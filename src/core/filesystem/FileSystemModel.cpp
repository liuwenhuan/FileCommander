#include "FileSystemModel.h"

#include <QCollator>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>

#include "IconCache.h"

namespace {

QVector<FileInfo> scanDirectory(const QString &path, bool showHidden) {
    QVector<FileInfo> result;
    QDir dir(path);
    QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
    if (showHidden)
        filters |= QDir::Hidden;

    const QFileInfoList entries = dir.entryInfoList(filters);
    result.reserve(entries.size());
    for (const QFileInfo &qfi : entries)
        result.append(FileInfo(qfi.absoluteFilePath()));
    return result;
}

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

} // namespace

FileSystemModel::FileSystemModel(QObject *parent) : QAbstractTableModel(parent) {
    connect(&m_watcher, &QFutureWatcher<QVector<FileInfo>>::finished, this,
            &FileSystemModel::onScanFinished);
}

void FileSystemModel::setRootPath(const QString &path) {
    m_rootPath = path;
    m_nameFilter.clear();     // a fresh directory always starts unfiltered
    m_dirSizes.clear();       // computed folder sizes don't survive a rescan
    m_compareStatus.clear();  // comparison highlights are stale after a rescan
    emit loadStarted();
    QFuture<QVector<FileInfo>> future =
        QtConcurrent::run(scanDirectory, path, m_showHidden);
    m_watcher.setFuture(future);
}

void FileSystemModel::setShowHiddenFiles(bool show) {
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    if (!m_rootPath.isEmpty())
        setRootPath(m_rootPath);
}

void FileSystemModel::onScanFinished() {
    beginResetModel();
    m_allEntries = m_watcher.result();
    m_hasParentEntry = QDir(m_rootPath).absolutePath() != QDir(m_rootPath).rootPath() &&
                        QDir::cleanPath(m_rootPath) != QDir::rootPath();
    sortEntries(); // sorts m_allEntries and rebuilds the visible m_entries
    endResetModel();
    emit loadFinished(m_entries.size());
}

bool FileSystemModel::matchesFilter(const QString &name, const QString &filter) {
    if (filter.isEmpty())
        return true;
    if (filter.contains(QLatin1Char('*')) || filter.contains(QLatin1Char('?'))) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(filter),
                                     QRegularExpression::CaseInsensitiveOption);
        return re.match(name).hasMatch();
    }
    return name.contains(filter, Qt::CaseInsensitive);
}

void FileSystemModel::applyFilter() {
    if (m_nameFilter.isEmpty()) {
        m_entries = m_allEntries;
        return;
    }
    m_entries.clear();
    m_entries.reserve(m_allEntries.size());
    for (const FileInfo &entry : m_allEntries)
        if (matchesFilter(entry.name(), m_nameFilter))
            m_entries.append(entry);
}

void FileSystemModel::setNameFilter(const QString &filter) {
    if (m_nameFilter == filter)
        return;
    beginResetModel();
    m_nameFilter = filter;
    applyFilter();
    endResetModel();
}

qint64 FileSystemModel::directorySize(const QString &path) {
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

void FileSystemModel::setCompareStatus(const QHash<QString, int> &statusByName) {
    beginResetModel();
    m_compareStatus = statusByName;
    endResetModel();
}

QHash<QString, int> FileSystemModel::compareStatuses(const QHash<QString, QDateTime> &self,
                                                      const QHash<QString, QDateTime> &other) {
    QHash<QString, int> result;
    for (auto it = self.constBegin(); it != self.constEnd(); ++it) {
        if (!other.contains(it.key()))
            result.insert(it.key(), CompareUnique);
        else if (it.value() > other.value(it.key()))
            result.insert(it.key(), CompareNewer);
        else
            result.insert(it.key(), CompareOlder);
    }
    return result;
}

void FileSystemModel::clearCompareStatus() {
    if (m_compareStatus.isEmpty())
        return;
    beginResetModel();
    m_compareStatus.clear();
    endResetModel();
}

void FileSystemModel::setComputedDirSize(const QString &path, qint64 bytes) {
    m_dirSizes.insert(path, bytes);
    // Repaint the Size cell of the matching visible row, if it's shown.
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).path() == path) {
            const int row = m_hasParentEntry ? i + 1 : i;
            const QModelIndex idx = index(row, SizeColumn);
            emit dataChanged(idx, idx, {Qt::DisplayRole});
            break;
        }
    }
}

bool FileSystemModel::isParentEntry(int row) const {
    return m_hasParentEntry && row == 0;
}

FileInfo FileSystemModel::fileInfoAt(int row) const {
    if (isParentEntry(row))
        return FileInfo::makeParentEntry(QDir(m_rootPath).absoluteFilePath(".."));
    int idx = m_hasParentEntry ? row - 1 : row;
    if (idx < 0 || idx >= m_entries.size())
        return FileInfo();
    return m_entries.at(idx);
}

int FileSystemModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return m_entries.size() + (m_hasParentEntry ? 1 : 0);
}

int FileSystemModel::columnCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant FileSystemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};

    const FileInfo info = fileInfoAt(index.row());
    if (!info.isValid())
        return {};

    if (role == FileInfoRole)
        return QVariant::fromValue(info.path());
    if (role == IsDirRole)
        return info.isDir();
    if (role == Qt::DecorationRole && index.column() == NameColumn)
        return IconCache::instance().iconFor(info);

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case NameColumn:
            return info.name();
        case ExtColumn:
            return info.isDir() ? QString() : info.suffix();
        case SizeColumn:
            if (info.isDir()) {
                auto it = m_dirSizes.constFind(info.path());
                return it != m_dirSizes.constEnd() ? humanSize(it.value())
                                                     : QStringLiteral("<DIR>");
            }
            return humanSize(info.size());
        case ModifiedColumn:
            return info.modified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        case PermissionsColumn:
            return info.permissionsString();
        default:
            return {};
        }
    }

    if (role == Qt::TextAlignmentRole && index.column() == SizeColumn)
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);

    if (role == Qt::ForegroundRole && !m_compareStatus.isEmpty()) {
        switch (m_compareStatus.value(info.name(), CompareNone)) {
        case CompareNewer:
            return QColor(0xc0, 0x39, 0x2b); // red: newer than the other panel
        case CompareUnique:
            return QColor(0x27, 0x7a, 0x46); // green: only on this side
        default:
            break;
        }
    }

    return {};
}

QVariant FileSystemModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);

    switch (section) {
    case NameColumn:
        return QObject::tr("Name");
    case ExtColumn:
        return QObject::tr("Ext");
    case SizeColumn:
        return QObject::tr("Size");
    case ModifiedColumn:
        return QObject::tr("Modified");
    case PermissionsColumn:
        return QObject::tr("Permissions");
    default:
        return {};
    }
}

void FileSystemModel::sort(int column, Qt::SortOrder order) {
    m_sortColumn = column;
    m_sortOrder = order;
    beginResetModel();
    sortEntries();
    endResetModel();
}

void FileSystemModel::sortEntries() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    std::sort(m_allEntries.begin(), m_allEntries.end(), [&](const FileInfo &a, const FileInfo &b) {
        // Directories always sort before files, regardless of sort column.
        if (a.isDir() != b.isDir())
            return a.isDir();

        int cmp = 0;
        switch (m_sortColumn) {
        case SizeColumn: {
            // Use a computed folder size where we have one, so sorting by size
            // orders directories by their real footprint, not the tiny inode.
            auto effectiveSize = [this](const FileInfo &fi) {
                auto it = m_dirSizes.constFind(fi.path());
                return it != m_dirSizes.constEnd() ? it.value() : fi.size();
            };
            const qint64 sa = effectiveSize(a);
            const qint64 sb = effectiveSize(b);
            cmp = (sa < sb) ? -1 : (sa > sb ? 1 : 0);
            break;
        }
        case ModifiedColumn:
            cmp = a.modified() < b.modified() ? -1 : (a.modified() > b.modified() ? 1 : 0);
            break;
        case ExtColumn:
            cmp = collator.compare(a.suffix(), b.suffix());
            break;
        default:
            cmp = collator.compare(a.name(), b.name());
            break;
        }
        if (cmp == 0)
            cmp = collator.compare(a.name(), b.name());
        return m_sortOrder == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
    });
    applyFilter(); // keep the visible subset in sync with the sorted source
}

Qt::ItemFlags FileSystemModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (!index.isValid())
        return f;
    // Editing (F2 inline rename) is only offered on the Name cell of real
    // entries -- never on "..". Views must still call edit() explicitly
    // (see FileListView/MainWindow::renameCurrent): NoEditTriggers is set
    // globally so a stray click/keystroke never starts a rename by
    // accident.
    if (index.column() == NameColumn && !isParentEntry(index.row()))
        f |= Qt::ItemIsEditable;

    // Without ItemIsDragEnabled, QAbstractItemView refuses to start a drag
    // at all -- it never even calls the view's startDrag() override. ".."
    // isn't a real draggable entry, so leave it out.
    if (!isParentEntry(index.row()))
        f |= Qt::ItemIsDragEnabled;
    f |= Qt::ItemIsDropEnabled;

    return f;
}

bool FileSystemModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (role != Qt::EditRole || index.column() != NameColumn || isParentEntry(index.row()))
        return false;

    const QString newName = value.toString().trimmed();
    if (newName.isEmpty())
        return false;

    const FileInfo info = fileInfoAt(index.row());
    if (!info.isValid() || newName == info.name())
        return false;

    const QString destPath = QDir(m_rootPath).filePath(newName);
    if (QFileInfo::exists(destPath)) {
        emit renameFailed(tr("%1 already exists").arg(newName));
        return false;
    }
    const QString oldPath = info.path();
    if (!QDir().rename(oldPath, destPath)) {
        emit renameFailed(tr("Failed to rename %1").arg(info.name()));
        return false;
    }

    emit renamed(oldPath, destPath);
    setRootPath(m_rootPath); // reload to pick up the new name/sort position
    return true;
}
