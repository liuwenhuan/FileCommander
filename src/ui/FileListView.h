#pragma once

#include <QTableView>

// QTableView with the header/selection behavior a file panel needs
// (stretch the Name column, select whole rows, keyboard-driven), plus
// drag-and-drop as both a source (drag files out to another panel, the
// desktop, or another app) and a target (drop files in from any of
// those, including a plain filesystem drag from another file manager).
class FileListView : public QTableView {
    Q_OBJECT

public:
    enum class DropActionKind { Copy, Move, Link };
    Q_ENUM(DropActionKind)

    explicit FileListView(QWidget *parent = nullptr);

    // Note: QAbstractItemView already provides an `activated(QModelIndex)`
    // signal (fired on double-click/Enter) — no need to redeclare it here.

    void setModel(QAbstractItemModel *model) override;

    // Type-to-jump always matches the Name column, regardless of which cell
    // is currently focused.
    void keyboardSearch(const QString &search) override;

signals:
    // kind is decided from live modifier keys at drop time (not drag
    // start): in-panel default=Move, Ctrl=Copy, Shift=Link; cross-panel
    // (or from outside the app) default=Copy, Ctrl=Move.
    void filesDropped(const QStringList &sourcePaths, const QString &destDir,
                       FileListView::DropActionKind kind);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QString destinationDirForDrop(const QPoint &pos) const;
};
