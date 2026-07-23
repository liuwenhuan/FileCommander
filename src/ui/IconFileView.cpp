#include "IconFileView.h"

#include <QApplication>
#include <QDrag>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QSet>
#include <QUrl>

#include "DragPixmap.h"
#include "FileSystemModel.h"

IconFileView::IconFileView(QWidget *parent) : QListView(parent) {
    // Drag source + drop target. The view mode, icon size and model are set up
    // by FilePanel, not here.
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
}

void IconFileView::startDrag(Qt::DropActions supportedActions) {
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return;

    // IconMode selects items, not rows, so selectedRows() is empty here.
    // selectedIndexes() can also repeat a row across columns, so de-dup by row.
    QList<QUrl> urls;
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
        urls.append(QUrl::fromLocalFile(fsModel->fileInfoAt(idx.row()).path()));
    }
    if (urls.isEmpty())
        return;

    auto *mimeData = new QMimeData;
    mimeData->setUrls(urls);

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    // Show what's actually being dragged: the first item's icon, plus a stacked
    // pile + count badge for a multi-item drag.
    const QIcon icon =
        fsModel->index(firstIdx.row(), FileSystemModel::NameColumn).data(Qt::DecorationRole).value<QIcon>();
    drag->setPixmap(ttc::makeDragPixmap(icon, urls.size(), devicePixelRatioF()));
    drag->setHotSpot(QPoint(12, 12));
    drag->exec(supportedActions, Qt::CopyAction);
}

void IconFileView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void IconFileView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls())
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

    emit filesDropped(sourcePaths, destDir, kind);
    event->acceptProposedAction();
}
