#pragma once

#include <QTableView>

// QTableView with the header/selection behavior a file panel needs
// (stretch the Name column, select whole rows, keyboard-driven).
class FileListView : public QTableView {
    Q_OBJECT

public:
    explicit FileListView(QWidget *parent = nullptr);

    // Note: QAbstractItemView already provides an `activated(QModelIndex)`
    // signal (fired on double-click/Enter) — no need to redeclare it here.

    void setModel(QAbstractItemModel *model) override;

protected:
    void keyPressEvent(QKeyEvent *event) override;
};
