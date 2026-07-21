#pragma once

#include <QPersistentModelIndex>
#include <QTableView>
#include <QVector>

class QTimer;

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

    // Marks the owning panel active/inactive. Drives the "panelActive" dynamic
    // property that the theme QSS uses to soften the selection colour on the
    // inactive panel, so the active panel's cursor row stands out.
    void setPanelActive(bool active);

    // Sorts by a header column (toggles direction on re-click). Called by the
    // header view on a section click, since the DTK style swallows sectionClicked.
    void sortByHeaderSection(int column);

    // Marks the columns as user-controlled (e.g. a persisted header layout was
    // restored), so auto-fit-to-content on directory load is suppressed and the
    // widths are left exactly as they are.
    void markColumnsManual() { m_columnsManual = true; }

signals:
    // kind is decided from live modifier keys at drop time (not drag
    // start): in-panel default=Move, Ctrl=Copy, Shift=Link; cross-panel
    // (or from outside the app) default=Copy, Ctrl=Move.
    void filesDropped(const QStringList &sourcePaths, const QString &destDir,
                       FileListView::DropActionKind kind);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    // Click-on-already-selected-name/ext starts an inline rename, the way most
    // file managers do -- deferred by the double-click interval so a
    // double-click still opens the file instead of renaming it.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QString destinationDirForDrop(const QPoint &pos) const;
    // Scales the columns so they fill the viewport width, preserving their
    // relative proportions (so a manual column drag is kept as a ratio).
    void stretchColumnsToFit();
    // Sizes each column to its content (+ header), then scales the set to fill
    // the viewport exactly (never overflows -> no horizontal scrollbar). Used on
    // directory load while the user hasn't manually adjusted the columns.
    void fitColumnsToContents();
    // Cheap per-step variant used mid-resize: only the last visible column
    // absorbs the width delta (one resizeSection instead of one per column).
    void stretchLastColumnOnly();
    // Right-click on the header: toggle which columns are shown.
    void showColumnMenu(const QPoint &pos);

    bool m_adjustingColumns = false; // guards against re-entrancy
    bool m_columnsManual = false;    // user (or restore) set widths -> no auto-fit
    QTimer *m_refitTimer = nullptr;  // debounces the full refit during resizes
    // Column proportions captured at the start of a resize burst. The per-step
    // last-column-only stretch skews the live ratios, so the settled refit
    // restores these instead of scaling the mutated ones.
    QVector<int> m_resizeBaseSizes;

    // Click-to-rename: the name/ext cell that was already the sole selection
    // when the mouse went down, plus the timer that fires the edit once we're
    // sure no double-click is following.
    QPersistentModelIndex m_renameClickIndex;
    QTimer *m_renameClickTimer = nullptr;
};
