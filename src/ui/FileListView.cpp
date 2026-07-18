#include "FileListView.h"

#include <QHeaderView>
#include <QKeyEvent>

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
}

void FileListView::setModel(QAbstractItemModel *model) {
    QTableView::setModel(model);
    // Section 0 only exists once the header has picked up the model's
    // column count, so this must run after setModel(), not in the ctor.
    if (model && model->columnCount() > 0) {
        horizontalHeader()->setStretchLastSection(false);
        horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
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
