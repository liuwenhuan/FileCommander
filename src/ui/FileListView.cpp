#include "FileListView.h"

#include <QApplication>
#include <QDrag>
#include <QDropEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>

#include "FileSystemModel.h"

FileListView::FileListView(QWidget *parent) : QTableView(parent) {
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    verticalHeader()->hide();
    horizontalHeader()->setSortIndicatorShown(true);
    setSortingEnabled(true); // header clicks call FileSystemModel::sort() automatically

    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
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

        // Sensible starting widths; Name gets the lion's share.
        const int defaults[FileSystemModel::ColumnCount] = {280, 70, 100, 150, 110};
        for (int col = 0; col < model->columnCount() && col < FileSystemModel::ColumnCount; ++col)
            header->resizeSection(col, defaults[col]);
    }
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

void FileListView::startDrag(Qt::DropActions supportedActions) {
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
