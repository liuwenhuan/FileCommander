#pragma once

#include <QColor>
#include <QPalette>
#include <QPersistentModelIndex>
#include <QTableView>
#include <QVector>

class QTimer;
class QVariantAnimation;
class QFontMetrics;
class QWidget;
class FileProvider;

// QTableView with the header/selection behavior a file panel needs
// (stretch the Name column, select whole rows, keyboard-driven), plus
// drag-and-drop as both a source (drag files out to another panel, the
// desktop, or another app) and a target (drop files in from any of
// those, including a plain filesystem drag from another file manager).
class FileListView : public QTableView {
    Q_OBJECT
    Q_PROPERTY(QString dragFeedbackState READ dragFeedbackState)
    Q_PROPERTY(QColor dragFeedbackColor READ dragFeedbackColor)

public:
    enum class DropActionKind { Copy, Move, Link };
    Q_ENUM(DropActionKind)

    explicit FileListView(QWidget *parent = nullptr);

    QString dragFeedbackState() const;
    QColor dragFeedbackColor() const { return m_dragFeedbackColor; }

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

    // Per-column persisted "base" widths (the user's target widths at full
    // viewport). Info columns carry real values; the Name slot is informational
    // (recomputed as the flex remainder). Used by MainWindow to save each panel's
    // column layout independently.
    QVector<int> columnBaseWidths() const { return m_baseWidth; }

    // Atomically restores a persisted per-side layout: hidden-column mask, base
    // widths, and sort. Runs under the re-entrancy guard so none of the header
    // changes are mis-read as user drags. hiddenMask/sortCol < 0 mean "leave the
    // default". Widths <= 0 (and the Name slot) are ignored.
    void restoreColumnLayout(const QVector<int> &baseWidths, int hiddenMask, int sortCol,
                             Qt::SortOrder sortOrder);

    // Active sort, owned by the view (see m_sortColumn). setSort applies it to the
    // model and the header indicator.
    int sortColumn() const { return m_sortColumn; }
    Qt::SortOrder sortOrder() const { return m_sortOrder; }
    void setSort(int column, Qt::SortOrder order);

signals:
    // kind is decided from live modifier keys at drop time (not drag
    // start): in-panel default=Move, Ctrl=Copy, Shift=Link; cross-panel
    // (or from outside the app) default=Copy, Ctrl=Move.
    // srcProvider is the provider of the panel the drag started in (null for an
    // external drop), so the receiver knows the true source backend instead of
    // guessing from the path -- an archive's virtual "/file" is indistinguishable
    // from a real local path otherwise.
    void filesDropped(const QStringList &sourcePaths, const QString &destDir,
                       FileListView::DropActionKind kind, FileProvider *srcProvider);

    // Space was pressed on this row. Total Commander also counts a directory's
    // contents on Space, so the size replaces "<DIR>" in the list; the owning
    // panel decides whether the row is a directory worth counting.
    //
    // TC restricts this to an *unselected* directory, which works there because
    // clicking only moves the cursor and never selects. This view is
    // ExtendedSelection (clicking selects, the Windows model), so a directory
    // under the cursor is normally already selected and that rule would almost
    // never fire -- hence the deliberate divergence: Space on a directory counts
    // it whichever way the toggle goes.
    void rowSpaced(int row);

    // Ctrl+wheel asks the owning panel to use the same scale step as its
    // status-bar -/+ controls. Positive means larger, negative means smaller.
    void zoomRequested(int direction);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    // Click-on-already-selected-name/ext starts an inline rename, the way most
    // file managers do -- deferred by the double-click interval so a
    // double-click still opens the file instead of renaming it.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void updateGeometries() override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Invalidates the cached active/inactive selection palettes when the app
    // stylesheet (theme) changes, then re-applies the current one.
    void changeEvent(QEvent *event) override;
    // Repaints the whole old and new rows, not just the cell that gained or lost
    // the current index -- the cursor frame spans the row.
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;

private:
    // Paints the keyboard cursor over the current row as a single rectangle.
    // See the implementation for why this is not the delegate's job.
    void drawCursorRow();

    enum class DragFeedbackState { None, Accepted, Rejected, Success };

    QString destinationDirForDrop(const QPoint &pos) const;
    void showDragFeedback(DragFeedbackState state, int duration);
    void clearDragFeedback();
    QColor dragFeedbackColorFor(DragFeedbackState state) const;
    void setDragFeedbackColor(const QColor &color);
    // Lazily derives the active + inactive selection palettes from the theme QSS
    // once (a one-time double repolish), so setPanelActive can later switch
    // between them with a cheap setPalette() instead of a full repolish.
    void ensureSelectionPalettes();

    QPalette m_activePalette;         // selection colours for the active panel
    QPalette m_inactivePalette;       // softened selection colours when inactive
    bool m_selectionPalettesValid = false;
    bool m_panelActive = true;        // last applied active state
    bool m_panelActiveKnown = false;  // whether m_panelActive has been set yet

    // --- Column layout ------------------------------------------------------
    // Recomputes each info column's natural content width (measured once per
    // directory load, all rows) and the per-column compression floor, then
    // re-lays out. Hooked to the model's modelReset.
    void recomputeContentWidths();
    // Measures the widest DisplayRole string across ALL rows for a variable
    // column (Ext/Size/Type), so long values that scroll off-screen still fit.
    int measureVariableColumn(int column, const QFontMetrics &fm) const;
    // Distributes the current viewport width across the visible columns so they
    // fill it exactly (last column pinned to the right edge): Name absorbs slack,
    // then the shrink policy (Name to a 16-char floor, then priority-compress the
    // info columns) kicks in when cramped.
    void applyLayout(int protectedLeftColumn = -1, int protectedTargetColumn = -1);
    int columnLayoutWidth() const;
    void placeVerticalScrollBarBelowHeader();
    void scheduleVerticalScrollBarPlacement();
    // User dragged a column border: adjacent give-and-take keeping the total
    // pinned to the viewport (see the connect in the ctor).
    void onSectionResized(int logical, int oldSize, int newSize);
    // Double-click on a section's resize grip: fit an info column to its content
    // width (Name is the flex column, so it is skipped).
    void autoFitColumn(int logical);
    // Slot for QHeaderView::sectionHandleDoubleClicked, whose argument is the
    // column on the LEFT of the double-clicked divider. Fits the next visible
    // column on the RIGHT of that divider, leaving the left column unchanged.
    void autoFitColumnRightOfHandle(int handleLeftLogical);
    // Right-click on the header: toggle which columns are shown.
    void showColumnMenu(const QPoint &pos);

    bool m_adjustingColumns = false; // guards against re-entrancy in resizeSection

    // Per-column geometry, sized to the model's column count in setModel().
    QVector<int> m_baseWidth;    // persisted target width (info cols); Name = flex
    QVector<int> m_contentWidth; // measured natural content width (info cols)
    QVector<int> m_smartMin;     // compression floor = header text width + pad
    QVector<bool> m_userSet;     // user/restore set this base -> don't overwrite on load
    QVector<int> m_expectedProgrammaticWidth; // filters delayed resizeSection signals
    QVector<int> m_lastStableBaseWidth; // fallback when a platform omits handle press events
    QVector<int> m_preHandleResizeBaseWidth; // undo Qt resize side effects before handle auto-fit
    int m_nameFloor = 0;         // 16*avgChar + icon + pad, recomputed in applyLayout

    // Sort state owned by the view. The header's own sort-indicator is unreliable
    // as a source of truth: FileSystemModel::sort() resets the model
    // (begin/endResetModel), which clears the header indicator, so reading it back
    // to decide the next toggle direction breaks re-clicking the same column.
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // Click-to-rename: the name/ext cell that was already the sole selection
    // when the mouse went down, plus the timer that fires the edit once we're
    // sure no double-click is following.
    QPersistentModelIndex m_renameClickIndex;
    QTimer *m_renameClickTimer = nullptr;

    QVariantAnimation *m_dragFeedbackAnimation = nullptr;
    QTimer *m_dragFeedbackClearTimer = nullptr;
    QWidget *m_verticalScrollBarContainer = nullptr;
    bool m_scrollbarPlacementPending = false;
    DragFeedbackState m_dragFeedbackState = DragFeedbackState::None;
    QColor m_dragFeedbackColor;
};
