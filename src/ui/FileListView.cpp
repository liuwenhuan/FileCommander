#include "FileListView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QColor>
#include <QDrag>
#include <QDropEvent>
#include <QFont>
#include <QHeaderView>
#include <QAction>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include "FileSystemModel.h"

namespace {
// Header that paints its own (non-bold) section labels. The deepin (DTK)
// style draws header text bold and ignores the widget font / qss font-weight,
// so we bypass it here, matching the theme's section colours.
class PlainHeaderView : public QHeaderView {
public:
    explicit PlainHeaderView(QWidget *parent = nullptr) : QHeaderView(Qt::Horizontal, parent) {}

protected:
    void mousePressEvent(QMouseEvent *e) override {
        m_pressIndex = logicalIndexAt(e->pos().x());
        m_pressPos = e->pos();
        QHeaderView::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        QHeaderView::mouseReleaseEvent(e);
        // The DTK style doesn't emit sectionClicked, so trigger the sort here on
        // a genuine click: same section, negligible drag (not a resize/reorder).
        if (e->button() == Qt::LeftButton && m_pressIndex >= 0 &&
            logicalIndexAt(e->pos().x()) == m_pressIndex &&
            (e->pos() - m_pressPos).manhattanLength() < 4) {
            if (auto *view = qobject_cast<FileListView *>(parentWidget()))
                view->sortByHeaderSection(m_pressIndex);
        }
    }
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override {
        if (!rect.isValid() || !model())
            return;
        const bool light = palette().color(QPalette::Window).lightness() > 128;
        const QColor bg = light ? QColor(0xec, 0xec, 0xec) : QColor(0x23, 0x23, 0x23);
        const QColor fg = light ? QColor(0x20, 0x20, 0x20) : QColor(0xe0, 0xe0, 0xe0);
        const QColor border = light ? QColor(0xd0, 0xd0, 0xd0) : QColor(0x1a, 0x1a, 0x1a);

        painter->save();
        painter->fillRect(rect, bg);
        painter->setPen(border);
        painter->drawLine(rect.topRight(), rect.bottomRight());
        painter->drawLine(rect.bottomLeft(), rect.bottomRight());

        QFont f = font();
        f.setBold(false);
        f.setWeight(QFont::Normal);
        painter->setFont(f);
        painter->setPen(fg);
        const QString text =
            model()->headerData(logicalIndex, Qt::Horizontal, Qt::DisplayRole).toString();
        painter->drawText(rect.adjusted(8, 0, -20, 0), Qt::AlignVCenter | Qt::AlignHCenter, text);

        if (isSortIndicatorShown() && sortIndicatorSection() == logicalIndex) {
            const int cx = rect.right() - 12;
            const int cy = rect.center().y();
            QPolygon tri;
            if (sortIndicatorOrder() == Qt::AscendingOrder)
                tri << QPoint(cx - 4, cy + 2) << QPoint(cx + 4, cy + 2) << QPoint(cx, cy - 3);
            else
                tri << QPoint(cx - 4, cy - 2) << QPoint(cx + 4, cy - 2) << QPoint(cx, cy + 3);
            painter->setBrush(fg);
            painter->setPen(Qt::NoPen);
            painter->drawPolygon(tri);
        }
        painter->restore();
    }

private:
    int m_pressIndex = -1;
    QPoint m_pressPos;
};
} // namespace

FileListView::FileListView(QWidget *parent) : QTableView(parent) {
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    // Always reserve the vertical scrollbar's space. Panels share one view whose
    // model swaps on tab switch; with an auto-hiding scrollbar the viewport width
    // would jump by ~15px between a tab that needs the bar and one that doesn't,
    // and stretchColumnsToFit() (which fits columns to viewport()->width()) would
    // reflow the columns on every switch. Keeping the bar always-on holds the
    // viewport width constant so column widths stay put.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalHeader()->hide();
    setHorizontalHeader(new PlainHeaderView(this)); // non-bold, self-painted labels
    horizontalHeader()->setSortIndicatorShown(true);
    setSortingEnabled(true); // header clicks call FileSystemModel::sort() automatically
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showColumnMenu(pos); });

    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    m_renameClickTimer = new QTimer(this);
    m_renameClickTimer->setSingleShot(true);
    connect(m_renameClickTimer, &QTimer::timeout, this, [this]() {
        const QModelIndex idx(m_renameClickIndex);
        m_renameClickIndex = QModelIndex();
        // Only if it's still the sole selection (nothing changed while we waited).
        if (idx.isValid() && selectionModel() && selectionModel()->selectedRows().size() == 1
            && selectionModel()->isRowSelected(idx.row(), QModelIndex()))
            edit(idx);
    });
}

void FileListView::setModel(QAbstractItemModel *model) {
    QTableView::setModel(model);
    // Sections only exist once the header has the model's column count, so
    // this must run after setModel(), not in the ctor.
    if (model && model->columnCount() > 0) {
        QHeaderView *header = horizontalHeader();
        // Every column is user-resizable by dragging its edge, including
        // Name. No stretch section, so no column is locked.
        header->setStretchLastSection(false);
        for (int col = 0; col < model->columnCount(); ++col)
            header->setSectionResizeMode(col, QHeaderView::Interactive);

        // Starting proportions; stretchColumnsToFit() scales these to fill the
        // panel. Name gets the lion's share.
        // Name, Ext, Size, Modified, Type, Created, Permissions
        const int defaults[FileSystemModel::ColumnCount] = {280, 70, 100, 150, 90, 150, 110};
        for (int col = 0; col < model->columnCount() && col < FileSystemModel::ColumnCount; ++col)
            header->resizeSection(col, defaults[col]);

        // Default view: hide Created and Permissions (a persisted header state,
        // restored later by MainWindow, overrides this if the user changed it).
        header->setSectionHidden(FileSystemModel::CreatedColumn, true);
        header->setSectionHidden(FileSystemModel::PermissionsColumn, true);

        stretchColumnsToFit();

        // Re-arm click-to-sort after the model is set. setSortingEnabled(true)
        // alone left the header's sectionsClickable off (the replaced
        // PlainHeaderView / DTK style doesn't get it from setSortingEnabled), so
        // header clicks never reached FileSystemModel::sort(); set it explicitly.
        // Drive sorting from the header's own sectionClicked signal instead of
        // QTableView's internal sortIndicatorChanged wiring, which the DTK style
        // / replaced PlainHeaderView leaves disconnected (header clicks never
        // reached the model). Keep the indicator shown and sections clickable.
        // The header drives sorting itself (see PlainHeaderView::mouseRelease),
        // since the DTK style never emits sectionClicked. Disable QTableView's
        // own sort wiring to avoid double handling; keep the indicator shown.
        setSortingEnabled(false);
        horizontalHeader()->setSortIndicatorShown(true);
        horizontalHeader()->setSectionsClickable(true);
    }
}

void FileListView::sortByHeaderSection(int column) {
    QHeaderView *header = horizontalHeader();
    // Toggle direction when re-clicking the current sort column.
    Qt::SortOrder order = Qt::AscendingOrder;
    if (header->sortIndicatorSection() == column &&
        header->sortIndicatorOrder() == Qt::AscendingOrder)
        order = Qt::DescendingOrder;
    header->setSortIndicator(column, order);
    if (model())
        model()->sort(column, order);
}

void FileListView::showColumnMenu(const QPoint &pos) {
    if (!model())
        return;
    QHeaderView *header = horizontalHeader();
    QMenu menu(this);
    for (int c = 0; c < model()->columnCount(); ++c) {
        const QString label = model()->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        QAction *action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(c));
        if (c == FileSystemModel::NameColumn)
            action->setEnabled(false); // the Name column can't be hidden
        connect(action, &QAction::toggled, this, [this, c, header](bool on) {
            header->setSectionHidden(c, !on);
            stretchColumnsToFit();
        });
    }
    menu.exec(header->mapToGlobal(pos));
}

void FileListView::resizeEvent(QResizeEvent *event) {
    QTableView::resizeEvent(event);
    stretchColumnsToFit();
}

void FileListView::stretchColumnsToFit() {
    if (m_adjustingColumns)
        return;
    QHeaderView *header = horizontalHeader();
    if (!header || header->count() == 0)
        return;
    const int avail = viewport()->width();
    if (avail <= 0)
        return;

    QVector<int> cols;
    int total = 0;
    for (int c = 0; c < header->count(); ++c) {
        if (!header->isSectionHidden(c)) {
            cols.append(c);
            total += header->sectionSize(c);
        }
    }
    if (cols.isEmpty() || total <= 0)
        return;

    m_adjustingColumns = true;
    const double factor = static_cast<double>(avail) / total;
    int used = 0;
    for (int i = 0; i < cols.size(); ++i) {
        int width;
        if (i == cols.size() - 1)
            width = qMax(30, avail - used); // last column takes the remainder exactly
        else
            width = qMax(30, static_cast<int>(header->sectionSize(cols.at(i)) * factor));
        used += width;
        header->resizeSection(cols.at(i), width);
    }
    m_adjustingColumns = false;
}

void FileListView::setPanelActive(bool active) {
    if (property("panelActive").isValid() && property("panelActive").toBool() == active)
        return;
    setProperty("panelActive", active);
    // Re-run the QSS attribute selector so the selection colour updates now.
    style()->unpolish(this);
    style()->polish(this);
    viewport()->update();
}

void FileListView::keyboardSearch(const QString &search) {
    // Qt's default matches against the current index's column; force column 0
    // (Name) so typing always jumps by file name even after clicking a Size
    // or Date cell.
    const QModelIndex cur = currentIndex();
    if (cur.isValid() && cur.column() != 0)
        setCurrentIndex(cur.sibling(cur.row(), 0));
    QTableView::keyboardSearch(search);
}

void FileListView::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space) {
        const QModelIndex idx = currentIndex();
        if (idx.isValid() && selectionModel()) {
            selectionModel()->select(idx, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
            const QModelIndex next = idx.sibling(idx.row() + 1, idx.column());
            if (next.isValid())
                setCurrentIndex(next);
        }
        event->accept();
        return;
    }
    QTableView::keyPressEvent(event);
}

void FileListView::mousePressEvent(QMouseEvent *event) {
    m_renameClickTimer->stop();
    m_renameClickIndex = QModelIndex();

    // Remember whether this press landed on the name/ext cell of a row that was
    // already the sole selection -- that's the signal for "rename in place".
    // Evaluate before the base class mutates the selection.
    if (event->button() == Qt::LeftButton
        && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
        && state() != QAbstractItemView::EditingState && model() && selectionModel()) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid() && (idx.column() == FileSystemModel::NameColumn
                              || idx.column() == FileSystemModel::ExtColumn)
            && (model()->flags(idx) & Qt::ItemIsEditable)
            && selectionModel()->isRowSelected(idx.row(), QModelIndex())
            && selectionModel()->selectedRows().size() == 1)
            m_renameClickIndex = idx;
    }
    QTableView::mousePressEvent(event);
}

void FileListView::mouseReleaseEvent(QMouseEvent *event) {
    QTableView::mouseReleaseEvent(event);
    // Start the rename only if the button came up on the same cell it went
    // down on; defer by the double-click interval so a double-click (open)
    // cancels it first (see mouseDoubleClickEvent).
    if (m_renameClickIndex.isValid()
        && indexAt(event->pos()) == QModelIndex(m_renameClickIndex))
        m_renameClickTimer->start(QApplication::doubleClickInterval() + 10);
    else
        m_renameClickIndex = QModelIndex();
}

void FileListView::mouseDoubleClickEvent(QMouseEvent *event) {
    // A double-click opens the file -- never renames.
    m_renameClickTimer->stop();
    m_renameClickIndex = QModelIndex();
    QTableView::mouseDoubleClickEvent(event);
}

void FileListView::startDrag(Qt::DropActions supportedActions) {
    m_renameClickTimer->stop();
    m_renameClickIndex = QModelIndex();

    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return;

    QList<QUrl> urls;
    for (const QModelIndex &idx : selectionModel()->selectedRows()) {
        if (fsModel->isParentEntry(idx.row()))
            continue;
        urls.append(QUrl::fromLocalFile(fsModel->fileInfoAt(idx.row()).path()));
    }
    if (urls.isEmpty())
        return;

    auto *mimeData = new QMimeData;
    mimeData->setUrls(urls);

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->exec(supportedActions, Qt::CopyAction);
}

void FileListView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FileListView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

QString FileListView::destinationDirForDrop(const QPoint &pos) const {
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return QString();

    const QModelIndex idx = indexAt(pos);
    if (idx.isValid() && fsModel->data(idx, FileSystemModel::IsDirRole).toBool())
        return fsModel->data(idx, FileSystemModel::FileInfoRole).toString();
    return fsModel->rootPath();
}

void FileListView::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    QStringList sourcePaths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            sourcePaths.append(url.toLocalFile());
    }
    if (sourcePaths.isEmpty()) {
        event->ignore();
        return;
    }

    const QString destDir = destinationDirForDrop(event->pos());
    if (destDir.isEmpty()) {
        event->ignore();
        return;
    }

    const bool sameView = (event->source() == this);
    const bool fromAnotherPanel = event->source() != nullptr && !sameView;
    const auto modifiers = QApplication::keyboardModifiers();

    DropActionKind kind;
    if (sameView) {
        // In-panel drag: default move, Ctrl=copy, Shift=symlink.
        if (modifiers & Qt::ShiftModifier)
            kind = DropActionKind::Link;
        else if (modifiers & Qt::ControlModifier)
            kind = DropActionKind::Copy;
        else
            kind = DropActionKind::Move;
    } else {
        // Cross-panel or from outside the app: default copy, Ctrl=move.
        Q_UNUSED(fromAnotherPanel);
        kind = (modifiers & Qt::ControlModifier) ? DropActionKind::Move : DropActionKind::Copy;
    }

    // Dropping a selection onto itself (same dir) is a no-op, not an error.
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (sameView && fsModel && destDir == fsModel->rootPath()) {
        event->ignore();
        return;
    }

    emit filesDropped(sourcePaths, destDir, kind);
    event->acceptProposedAction();
}
