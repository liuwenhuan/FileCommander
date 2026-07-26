#pragma once

#include <QListView>

#include "FileListView.h"

class QTimer;

// QListView in IconMode for the thumbnail/icon file view, with the same
// drag-and-drop behavior as FileListView (drag files out to another panel,
// the desktop, or another app; drop them in from any of those). QListView's
// SelectItems selection means selectedRows() is always empty here, so the
// drag source builds its URL list from selectedIndexes() instead.
class IconFileView : public QListView {
    Q_OBJECT

public:
    explicit IconFileView(QWidget *parent = nullptr);

    // Rows whose items intersect the viewport right now, for a caller that
    // needs the range without waiting for the next visibleRangeSettled (e.g.
    // starting thumbnail work as soon as a directory finishes loading).
    // Returns false when there is no model or nothing is laid out yet.
    bool visibleRowRange(int *firstRow, int *lastRow) const { return visibleRows(firstRow, lastRow); }

signals:
    // Same signature as FileListView::filesDropped so MainWindow can wire both
    // views to the same slot. kind is decided from live modifier keys at drop
    // time (not drag start): in-panel default=Move, Ctrl=Copy, Shift=Link;
    // cross-panel (or from outside the app) default=Copy, Ctrl=Move.
    void filesDropped(const QStringList &sourcePaths, const QString &destDir,
                      FileListView::DropActionKind kind, FileProvider *srcProvider);

signals:
    // Emitted a moment after scrolling stops, with the rows now on screen.
    // Thumbnails are fetched over the network, so the rows the user has come to
    // rest on are the ones worth spending those fetches on -- and the rows they
    // flew past are worth abandoning. Deliberately not emitted during the
    // scroll: a fast drag would otherwise queue every row it swept over.
    void visibleRangeSettled(int firstRow, int lastRow);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QString destinationDirForDrop(const QPoint &pos) const;

    // Rows whose items intersect the viewport right now. Returns false when the
    // view has no model or nothing is laid out yet.
    bool visibleRows(int *firstRow, int *lastRow) const;

    void announceVisibleRange();

    QTimer *m_settleTimer; // restarted on every scroll; fires once movement stops
};
