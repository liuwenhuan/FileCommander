#include "SyncModel.h"

#include <QColor>
#include <QFileInfo>

namespace {

// Matches the file list's compare-mode palette so the two features speak the
// same visual language (FileSystemModel: red = newer, green = only on this side).
const QColor kNewerColor(0xc0, 0x39, 0x2b);
const QColor kUniqueColor(0x27, 0x7a, 0x46);
const QColor kConflictColor(0xd3, 0x8b, 0x1e);
const QColor kMutedColor(0x88, 0x88, 0x88);

QString humanSize(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

QString sizeOrDash(qint64 size) {
    return size < 0 ? QStringLiteral("—") : humanSize(size);
}

QString timeOrDash(const QDateTime &dt) {
    return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("—");
}

} // namespace

SyncModel::SyncModel(QObject *parent) : QAbstractTableModel(parent) {}

SyncModel::Direction SyncModel::defaultDirection(const SyncEntry &entry) {
    switch (entry.status) {
    case SyncEntry::Status::LeftOnly:
        return Direction::ToRight;
    case SyncEntry::Status::RightOnly:
        return Direction::ToLeft;
    case SyncEntry::Status::Same:
        return Direction::None;
    case SyncEntry::Status::Different:
        break;
    }

    // Both sides exist and differ: the newer one wins. When the timestamps are
    // within the comparison tolerance yet the contents differ (sizes disagree),
    // there is no defensible winner -- copying either way would silently destroy
    // the other side's edit, so the row asks the user instead of guessing.
    const qint64 delta = entry.leftModified.msecsTo(entry.rightModified);
    if (delta > DirectorySync::kTimeToleranceMs)
        return Direction::ToLeft;
    if (delta < -DirectorySync::kTimeToleranceMs)
        return Direction::ToRight;
    return Direction::Conflict;
}

void SyncModel::appendEntries(const QVector<SyncEntry> &entries) {
    if (entries.isEmpty())
        return;

    // Figure out how many of the incoming entries will actually be visible
    // before touching the view, so the insert range is exact.
    QVector<int> newVisible;
    newVisible.reserve(entries.size());
    int nextIndex = m_rows.size();
    for (const SyncEntry &e : entries) {
        if (m_showIdentical || e.status != SyncEntry::Status::Same)
            newVisible.append(nextIndex);
        ++nextIndex;
    }

    if (!newVisible.isEmpty())
        beginInsertRows({}, m_visible.size(), m_visible.size() + newVisible.size() - 1);

    for (const SyncEntry &e : entries) {
        Row row;
        row.entry = e;
        row.direction = defaultDirection(e);
        m_rows.append(row);
    }
    m_visible += newVisible;

    if (!newVisible.isEmpty())
        endInsertRows();

    recomputeSummary();
}

SyncModel::Direction SyncModel::directionAt(int visibleRow) const {
    const int src = sourceRow(visibleRow);
    return src < 0 ? Direction::None : m_rows.at(src).direction;
}

void SyncModel::clearRows() {
    beginResetModel();
    m_rows.clear();
    m_visible.clear();
    endResetModel();
    recomputeSummary();
}

int SyncModel::sourceRow(int visibleRow) const {
    if (visibleRow < 0 || visibleRow >= m_visible.size())
        return -1;
    return m_visible.at(visibleRow);
}

void SyncModel::rebuildVisible() {
    m_visible.clear();
    m_visible.reserve(m_rows.size());
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_showIdentical || m_rows.at(i).entry.status != SyncEntry::Status::Same)
            m_visible.append(i);
    }
}

void SyncModel::setShowIdentical(bool show) {
    if (m_showIdentical == show)
        return;
    beginResetModel();
    m_showIdentical = show;
    rebuildVisible();
    endResetModel();
}

void SyncModel::cycleDirection(int row) {
    const int src = sourceRow(row);
    if (src < 0)
        return;

    const Row &r = m_rows.at(src);
    // Identical rows have nothing to cycle through.
    if (r.entry.status == SyncEntry::Status::Same)
        return;

    const bool hasLeft = r.entry.leftSize >= 0;
    const bool hasRight = r.entry.rightSize >= 0;

    // Only offer directions that can actually be carried out: a file that exists
    // on one side only can be copied outward or skipped, nothing else.
    QVector<Direction> choices;
    if (hasLeft)
        choices.append(Direction::ToRight);
    if (hasRight)
        choices.append(Direction::ToLeft);
    choices.append(Direction::Skip);

    const int current = choices.indexOf(r.direction);
    // A Conflict row isn't in the list, so it lands on the first real choice.
    setDirection(row, choices.at((current + 1) % choices.size()));
}

void SyncModel::setDirection(int row, Direction direction) {
    const int src = sourceRow(row);
    if (src < 0 || m_rows.at(src).direction == direction)
        return;
    m_rows[src].direction = direction;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    recomputeSummary();
}

void SyncModel::setAllDirections(Direction direction) {
    bool changed = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        if (r.entry.status == SyncEntry::Status::Same)
            continue;
        // Never fabricate an impossible copy: "all left to right" must not try
        // to send a file that only exists on the right.
        if (direction == Direction::ToRight && r.entry.leftSize < 0)
            continue;
        if (direction == Direction::ToLeft && r.entry.rightSize < 0)
            continue;
        if (r.direction == direction)
            continue;
        r.direction = direction;
        changed = true;
    }
    if (!changed)
        return;
    if (!m_visible.isEmpty())
        emit dataChanged(index(0, 0), index(m_visible.size() - 1, ColumnCount - 1));
    recomputeSummary();
}

QVector<SyncModel::Row> SyncModel::actionableRows() const {
    QVector<Row> out;
    for (const Row &r : m_rows) {
        if (r.direction == Direction::ToRight || r.direction == Direction::ToLeft)
            out.append(r);
    }
    return out;
}

void SyncModel::recomputeSummary() {
    Summary s;
    for (const Row &r : m_rows) {
        switch (r.direction) {
        case Direction::ToRight:
            ++s.toRight;
            s.bytesToCopy += qMax<qint64>(0, r.entry.leftSize);
            break;
        case Direction::ToLeft:
            ++s.toLeft;
            s.bytesToCopy += qMax<qint64>(0, r.entry.rightSize);
            break;
        case Direction::Conflict:
            ++s.conflicts;
            break;
        case Direction::Skip:
            ++s.skipped;
            break;
        case Direction::None:
            ++s.identical;
            break;
        }
    }
    m_summary = s;
    emit summaryChanged();
}

QString SyncModel::explain(int row) const {
    const int src = sourceRow(row);
    if (src < 0)
        return {};
    const Row &r = m_rows.at(src);

    switch (r.direction) {
    case Direction::ToRight:
        return tr("Copy left → right. Click to change direction or skip.");
    case Direction::ToLeft:
        return tr("Copy right → left. Click to change direction or skip.");
    case Direction::Skip:
        return tr("Skipped — this file will not be touched. Click to choose a direction.");
    case Direction::None:
        return tr("Both sides match; nothing to do.");
    case Direction::Conflict:
        break;
    }
    return tr("Both sides were changed, or the clocks disagree: the contents differ but the "
              "timestamps look the same, so neither side is safe to assume newer. "
              "Click to pick a direction yourself.");
}

int SyncModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_visible.size();
}

int SyncModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SyncModel::data(const QModelIndex &index, int role) const {
    const int src = sourceRow(index.row());
    if (src < 0)
        return {};
    const Row &r = m_rows.at(src);
    const SyncEntry &e = r.entry;
    const bool hasLeft = e.leftSize >= 0;
    const bool hasRight = e.rightSize >= 0;
    // Show the bare file name; the relative directory is carried by the group
    // context and the tooltip, so deep trees don't smear the name column.
    const QString name = QFileInfo(e.relativePath).fileName();

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case LeftNameColumn:
            return hasLeft ? name : QStringLiteral("—");
        case LeftSizeColumn:
            return sizeOrDash(e.leftSize);
        case LeftTimeColumn:
            return hasLeft ? timeOrDash(e.leftModified) : QStringLiteral("—");
        case ActionColumn:
            return {}; // painted by SyncActionDelegate
        case RightNameColumn:
            return hasRight ? name : QStringLiteral("—");
        case RightSizeColumn:
            return sizeOrDash(e.rightSize);
        case RightTimeColumn:
            return hasRight ? timeOrDash(e.rightModified) : QStringLiteral("—");
        default:
            break;
        }
        return {};
    }

    if (role == Qt::ToolTipRole)
        return e.relativePath + QStringLiteral("\n") + explain(index.row());

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == LeftSizeColumn || index.column() == RightSizeColumn)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        if (index.column() == ActionColumn)
            return static_cast<int>(Qt::AlignCenter);
        return {};
    }

    if (role == Qt::ForegroundRole) {
        if (r.direction == Direction::Skip || e.status == SyncEntry::Status::Same)
            return kMutedColor;
        if (e.status == SyncEntry::Status::Different) {
            if (r.direction == Direction::Conflict)
                return kConflictColor;
            // Tint the side that is about to be overwritten, so the losing copy
            // is visible at a glance.
            const bool leftCol = index.column() <= LeftTimeColumn;
            const bool leftWins = r.direction == Direction::ToRight;
            if (leftCol == leftWins)
                return kNewerColor;
            return {};
        }
        // Left-only / right-only: green on the side that has the file.
        const bool leftCol = index.column() <= LeftTimeColumn;
        if ((leftCol && hasLeft && !hasRight) || (!leftCol && hasRight && !hasLeft))
            return kUniqueColor;
        return kMutedColor;
    }

    return {};
}

QVariant SyncModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case LeftNameColumn:
        return tr("Name");
    case LeftSizeColumn:
        return tr("Size");
    case LeftTimeColumn:
        return tr("Modified");
    case ActionColumn:
        return tr("Action");
    case RightNameColumn:
        return tr("Name");
    case RightSizeColumn:
        return tr("Size");
    case RightTimeColumn:
        return tr("Modified");
    default:
        break;
    }
    return {};
}
