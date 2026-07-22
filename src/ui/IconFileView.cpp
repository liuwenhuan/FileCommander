#include "IconFileView.h"

#include <QApplication>
#include <QDrag>
#include <QDropEvent>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QSet>
#include <QUrl>

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
    for (const QModelIndex &idx : selectionModel()->selectedIndexes()) {
        if (seenRows.contains(idx.row()))
            continue;
        seenRows.insert(idx.row());
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

void IconFileView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void IconFileView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
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
