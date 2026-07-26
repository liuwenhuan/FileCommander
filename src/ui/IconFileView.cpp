#include "IconFileView.h"

#include <QApplication>
#include <QDrag>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include "DragPixmap.h"
#include "ExternalPaths.h"
#include "FileSystemModel.h"

namespace {

// How long the view must stay still before its rows are treated as "what the
// user is looking at". Long enough not to fire mid-flick, short enough that
// letting go feels like it starts work immediately.
constexpr int kScrollSettleMs = 120;

} // namespace

IconFileView::IconFileView(QWidget *parent)
    : QListView(parent), m_settleTimer(new QTimer(this)) {
    // Drag source + drop target. The view mode, icon size and model are set up
    // by FilePanel, not here.
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(kScrollSettleMs);
    connect(m_settleTimer, &QTimer::timeout, this, &IconFileView::announceVisibleRange);
}

bool IconFileView::visibleRows(int *firstRow, int *lastRow) const {
    if (!model() || model()->rowCount() == 0)
        return false;

    // Probing the corners is not enough: in icon mode they usually land in the
    // spacing between items, where indexAt() reports nothing. Sample a grid of
    // points across the viewport and keep the extremes that hit an item.
    const QRect area = viewport()->rect();
    if (area.isEmpty())
        return false;

    int lo = -1;
    int hi = -1;
    constexpr int kSamplesX = 8;
    constexpr int kSamplesY = 8;
    for (int iy = 0; iy <= kSamplesY; ++iy) {
        const int y = area.top() + iy * (area.height() - 1) / kSamplesY;
        for (int ix = 0; ix <= kSamplesX; ++ix) {
            const int x = area.left() + ix * (area.width() - 1) / kSamplesX;
            const QModelIndex idx = indexAt(QPoint(x, y));
            if (!idx.isValid())
                continue;
            if (lo < 0 || idx.row() < lo)
                lo = idx.row();
            if (idx.row() > hi)
                hi = idx.row();
        }
    }
    if (lo < 0)
        return false;

    *firstRow = lo;
    *lastRow = hi;
    return true;
}

void IconFileView::announceVisibleRange() {
    int first = 0, last = 0;
    if (visibleRows(&first, &last))
        emit visibleRangeSettled(first, last);
}

void IconFileView::scrollContentsBy(int dx, int dy) {
    QListView::scrollContentsBy(dx, dy);
    // Restart rather than fire: during a continuous scroll this keeps pushing
    // the deadline out, so the work starts once, where the user stopped.
    if (dx != 0 || dy != 0)
        m_settleTimer->start();
}

void IconFileView::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    // A resize relays out the grid, so a different set of rows is on screen.
    m_settleTimer->start();
}

void IconFileView::startDrag(Qt::DropActions supportedActions) {
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return;

    // IconMode selects items, not rows, so selectedRows() is empty here.
    // selectedIndexes() can also repeat a row across columns, so de-dup by row.
    QStringList paths;
    QSet<int> seenRows;
    QModelIndex firstIdx;
    for (const QModelIndex &idx : selectionModel()->selectedIndexes()) {
        if (seenRows.contains(idx.row()))
            continue;
        seenRows.insert(idx.row());
        if (fsModel->isParentEntry(idx.row()))
            continue;
        // The drag icon shows the topmost (first-listed) selected item.
        if (!firstIdx.isValid() || idx.row() < firstIdx.row())
            firstIdx = idx;
        paths.append(fsModel->fileInfoAt(idx.row()).path());
    }
    if (paths.isEmpty())
        return;

    auto *mimeData = new QMimeData;
    // Same split as FileListView::startDrag: the private format holds the
    // backend's own paths for an in-app drop, while the public URL list only
    // ever names something that really resolves outside this process.
    fc::setPathPayload(mimeData, fsModel->provider(), paths, /*cut=*/false);

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    // Show what's actually being dragged: the first item's icon, plus a stacked
    // pile + count badge for a multi-item drag.
    const QIcon icon =
        fsModel->index(firstIdx.row(), FileSystemModel::NameColumn).data(Qt::DecorationRole).value<QIcon>();
    drag->setPixmap(ttc::makeDragPixmap(icon, paths.size(), devicePixelRatioF()));
    drag->setHotSpot(QPoint(12, 12));
    drag->exec(supportedActions, Qt::CopyAction);
}

void IconFileView::dragEnterEvent(QDragEnterEvent *event) {
    if (fc::hasIncomingPaths(event->mimeData()))
        event->acceptProposedAction();
}

void IconFileView::dragMoveEvent(QDragMoveEvent *event) {
    if (fc::hasIncomingPaths(event->mimeData()))
        event->acceptProposedAction();
}

void IconFileView::dragLeaveEvent(QDragLeaveEvent *event) {
    // We draw no drop indicator, so skip QAbstractItemView's dragLeave handler,
    // which repaints the entire viewport. That full repaint fired every time a
    // fast drag crossed out of this view (e.g. over the splitter into the other
    // panel), causing a visible stutter. Accepting without a repaint is enough.
    event->accept();
}

QString IconFileView::destinationDirForDrop(const QPoint &pos) const {
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return QString();

    const QModelIndex idx = indexAt(pos);
    if (idx.isValid() && fsModel->data(idx, FileSystemModel::IsDirRole).toBool())
        return fsModel->data(idx, FileSystemModel::FileInfoRole).toString();
    return fsModel->rootPath();
}

void IconFileView::dropEvent(QDropEvent *event) {
    // Prefer the private format: a drag out of a network or archive panel puts
    // the backend's real paths there and nothing usable in the public URL list.
    const QStringList sourcePaths = fc::incomingPaths(event->mimeData());
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

    FileListView::DropActionKind kind;
    if (sameView) {
        // In-panel drag: default move, Ctrl=copy, Shift=symlink.
        if (modifiers & Qt::ShiftModifier)
            kind = FileListView::DropActionKind::Link;
        else if (modifiers & Qt::ControlModifier)
            kind = FileListView::DropActionKind::Copy;
        else
            kind = FileListView::DropActionKind::Move;
    } else {
        // Cross-panel or from outside the app: default copy, Ctrl=move.
        Q_UNUSED(fromAnotherPanel);
        kind = (modifiers & Qt::ControlModifier) ? FileListView::DropActionKind::Move
                                                 : FileListView::DropActionKind::Copy;
    }

    // Dropping a selection onto itself (same dir) is a no-op, not an error.
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (sameView && fsModel && destDir == fsModel->rootPath()) {
        event->ignore();
        return;
    }

    FileProvider *srcProvider = nullptr;
    if (auto *srcView = qobject_cast<QAbstractItemView *>(event->source()))
        if (auto *srcModel = qobject_cast<FileSystemModel *>(srcView->model()))
            srcProvider = srcModel->provider();
    emit filesDropped(sourcePaths, destDir, kind, srcProvider);
    event->acceptProposedAction();
}
