#pragma once

#include <QListView>

#include "FileListView.h"

// QListView in IconMode for the thumbnail/icon file view, with the same
// drag-and-drop behavior as FileListView (drag files out to another panel,
// the desktop, or another app; drop them in from any of those). QListView's
// SelectItems selection means selectedRows() is always empty here, so the
// drag source builds its URL list from selectedIndexes() instead.
class IconFileView : public QListView {
    Q_OBJECT

public:
    explicit IconFileView(QWidget *parent = nullptr);

signals:
    // Same signature as FileListView::filesDropped so MainWindow can wire both
    // views to the same slot. kind is decided from live modifier keys at drop
    // time (not drag start): in-panel default=Move, Ctrl=Copy, Shift=Link;
    // cross-panel (or from outside the app) default=Copy, Ctrl=Move.
    void filesDropped(const QStringList &sourcePaths, const QString &destDir,
                      FileListView::DropActionKind kind);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QString destinationDirForDrop(const QPoint &pos) const;
};
