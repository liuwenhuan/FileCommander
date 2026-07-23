#include "FileListView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QColor>
#include <QDrag>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QPalette>
#include <QHeaderView>
#include <QAction>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QStringList>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QStyledItemDelegate>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include "DragPixmap.h"
#include "FileSystemModel.h"

namespace {
// Baseline column proportions (Name, Ext, Size, Modified, Type, Created,
// Permissions). Used as the initial layout and as a per-column floor for the
// content-aware auto-fit so no column ever collapses.
constexpr int kDefaultColWidths[FileSystemModel::ColumnCount] = {280, 70, 100, 150,
                                                                 90,  150, 110};

// Header that paints its own (non-bold) section labels. The deepin (DTK)
// style draws header text bold and ignores the widget font / qss font-weight,
// so we bypass it here, matching the theme's section colours.
class PlainHeaderView : public QHeaderView {
public:
    explicit PlainHeaderView(QWidget *parent = nullptr) : QHeaderView(Qt::Horizontal, parent) {}

protected:
    void mousePressEvent(QMouseEvent *e) override {
        m_pressIndex = logicalIndexAt(e->pos().x());
        m_pressPos = e->pos();
        QHeaderView::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        QHeaderView::mouseReleaseEvent(e);
        // The DTK style doesn't emit sectionClicked, so trigger the sort here on
        // a genuine click on the label: same section, negligible drag (not a
        // resize/reorder), and NOT on a section border -- a click/double-click on
        // the resize grip is a width adjustment, never a sort.
        if (e->button() == Qt::LeftButton && m_pressIndex >= 0 &&
            logicalIndexAt(e->pos().x()) == m_pressIndex &&
            (e->pos() - m_pressPos).manhattanLength() < 4 && !onResizeGrip(m_pressPos.x())) {
            if (auto *view = qobject_cast<FileListView *>(parentWidget()))
                view->sortByHeaderSection(m_pressIndex);
        }
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        // Let the base emit sectionHandleDoubleClicked (auto-fit) for a grip
        // double-click. A double-click -- whether it auto-fits a column or lands
        // on a label -- must not sort: clear the press index so the trailing
        // release doesn't. (Auto-fit also shifts the borders, which would
        // otherwise make that release look like a label click.)
        QHeaderView::mouseDoubleClickEvent(e);
        m_pressIndex = -1;
    }
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override {
        if (!rect.isValid() || !model())
            return;
        const bool light = palette().color(QPalette::Window).lightness() > 128;
        const QColor bg = light ? QColor(0xec, 0xec, 0xec) : QColor(0x23, 0x23, 0x23);
        const QColor fg = light ? QColor(0x20, 0x20, 0x20) : QColor(0xe0, 0xe0, 0xe0);
        // Divider between header sections. In dark mode 0x1a was nearly
        // indistinguishable from the 0x23 header background, so the column
        // separators vanished; use a mid grey that reads clearly on both.
        const QColor border = light ? QColor(0xd0, 0xd0, 0xd0) : QColor(0x50, 0x50, 0x50);

        painter->save();
        painter->fillRect(rect, bg);
        painter->setPen(border);
        painter->drawLine(rect.topRight(), rect.bottomRight());
        painter->drawLine(rect.bottomLeft(), rect.bottomRight());

        QFont f = font();
        f.setBold(false);
        f.setWeight(QFont::Normal);
        painter->setFont(f);
        painter->setPen(fg);
        const QString text =
            model()->headerData(logicalIndex, Qt::Horizontal, Qt::DisplayRole).toString();
        painter->drawText(rect.adjusted(8, 0, -20, 0), Qt::AlignVCenter | Qt::AlignHCenter, text);

        if (isSortIndicatorShown() && sortIndicatorSection() == logicalIndex) {
            const int cx = rect.right() - 12;
            const int cy = rect.center().y();
            QPolygon tri;
            if (sortIndicatorOrder() == Qt::AscendingOrder)
                tri << QPoint(cx - 4, cy + 2) << QPoint(cx + 4, cy + 2) << QPoint(cx, cy - 3);
            else
                tri << QPoint(cx - 4, cy - 2) << QPoint(cx + 4, cy - 2) << QPoint(cx, cy + 3);
            painter->setBrush(fg);
            painter->setPen(Qt::NoPen);
            painter->drawPolygon(tri);
        }
        painter->restore();
    }

private:
    // True when x is within the resize grip of any visible section boundary, so
    // a click there is a width drag/auto-fit rather than a label click.
    bool onResizeGrip(int x) const {
        const int grip =
            qMax(5, style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this));
        for (int i = 0; i < count(); ++i) {
            if (isSectionHidden(i))
                continue;
            const int right = sectionViewportPosition(i) + sectionSize(i);
            if (qAbs(x - right) <= grip)
                return true;
        }
        return false;
    }

    int m_pressIndex = -1;
    QPoint m_pressPos;
};

// Paints file-list cells directly (background, icon, text) with a QPainter
// instead of letting the item go through QStyleSheetStyle. The app sets a global
// stylesheet for theming, which otherwise routes every cell through the CSS
// item box-model machinery -- cheap once, but during an interactive column /
// splitter resize the whole viewport of BOTH panels repaints per mouse step,
// and the per-cell CSS cost is what visibly lagged the drag. All colours come
// from the (theme-populated) palette, so light/dark and the active/inactive
// selection tint still work; the model's ForegroundRole (compare-by-time
// red/green) is honoured too.
class FileItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index); // fills text, icon, font, displayAlignment

        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette &pal = opt.palette;

        painter->save();
        painter->setClipRect(opt.rect);

        // Row background. Selected rows use palette Highlight, which the
        // [panelActive] stylesheet rule swaps between the active/inactive tint;
        // everything else uses the base colour.
        painter->fillRect(opt.rect, selected ? pal.highlight() : pal.base());

        QRect r = opt.rect.adjusted(4, 0, -4, 0);

        // Decoration (only the Name column carries an icon).
        if (!opt.icon.isNull()) {
            const int sz = opt.decorationSize.height() > 0 ? opt.decorationSize.height() : 16;
            const QRect ir(r.left(), r.top() + (r.height() - sz) / 2, sz, sz);
            opt.icon.paint(painter, ir, Qt::AlignCenter,
                           selected ? QIcon::Selected : QIcon::Normal);
            r.setLeft(ir.right() + 4);
        }

        if (r.width() > 0 && !opt.text.isEmpty()) {
            // Text colour: selected text, else the model's ForegroundRole
            // (compare red/green), else the normal text colour.
            QColor fg = pal.text().color();
            if (selected) {
                fg = pal.highlightedText().color();
            } else {
                const QVariant v = index.data(Qt::ForegroundRole);
                if (v.canConvert<QColor>())
                    fg = qvariant_cast<QColor>(v);
            }
            painter->setPen(fg);
            painter->setFont(opt.font);
            const QString text =
                opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, r.width());
            painter->drawText(r, opt.displayAlignment, text);
        }

        painter->restore();
    }
};
} // namespace

FileListView::FileListView(QWidget *parent) : QTableView(parent) {
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Zebra striping is disabled: the themes set alternate-background-color equal
    // to the base, so the alternating-row machinery only added per-row style work
    // on every repaint (costly during an interactive resize) for no visual gain.
    setAlternatingRowColors(false);
    setShowGrid(false);
    setWordWrap(false);
    // Direct-painting delegate: keeps cells off QStyleSheetStyle's slow per-cell
    // CSS path so an interactive column/splitter resize stays smooth.
    setItemDelegate(new FileItemDelegate(this));
    // Always reserve the vertical scrollbar's space. Panels share one view whose
    // model swaps on tab switch; with an auto-hiding scrollbar the viewport width
    // would jump by ~15px between a tab that needs the bar and one that doesn't,
    // and applyLayout() (which fits columns to viewport()->width()) would
    // reflow the columns on every switch. Keeping the bar always-on holds the
    // viewport width constant so column widths stay put.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Columns are always managed to fill the viewport exactly (applyLayout), so
    // a horizontal scrollbar is never wanted -- turn it off so the last column's
    // right edge stays pinned (Qt otherwise flashes the bar at the exact-fit
    // boundary). In the extreme-narrow case the last column simply clips.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalHeader()->hide();
    setHorizontalHeader(new PlainHeaderView(this)); // non-bold, self-painted labels
    horizontalHeader()->setSortIndicatorShown(true);
    setSortingEnabled(true); // header clicks call FileSystemModel::sort() automatically
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showColumnMenu(pos); });
    // A user drag of a column border does adjacent give-and-take (the dragged
    // column and the next visible one trade width) so the total stays pinned to
    // the viewport and the last column's right edge never moves. Programmatic
    // resizes set m_adjustingColumns and are ignored here.
    connect(horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int logical, int oldSize, int newSize) {
                onSectionResized(logical, oldSize, newSize);
            });
    // Double-click a divider -> auto-fit the column on its RIGHT to content.
    connect(horizontalHeader(), &QHeaderView::sectionHandleDoubleClicked, this,
            &FileListView::autoFitColumnRightOfHandle);

    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    m_renameClickTimer = new QTimer(this);
    m_renameClickTimer->setSingleShot(true);
    connect(m_renameClickTimer, &QTimer::timeout, this, [this]() {
        const QModelIndex idx(m_renameClickIndex);
        m_renameClickIndex = QModelIndex();
        // Only if it's still the sole selection (nothing changed while we waited).
        if (idx.isValid() && selectionModel() && selectionModel()->selectedRows().size() == 1
            && selectionModel()->isRowSelected(idx.row(), QModelIndex()))
            edit(idx);
    });
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
        // A hidden section otherwise keeps a phantom minimumSectionSize slot in
        // the layout (reports size 0 yet still offsets later columns, leaving a
        // blank gap before the next visible column). Allow a zero minimum so a
        // hidden column truly occupies nothing; applyLayout enforces per-column
        // smart-mins on the *visible* columns, so this doesn't let them collapse.
        header->setMinimumSectionSize(0);
        for (int col = 0; col < model->columnCount(); ++col)
            header->setSectionResizeMode(col, QHeaderView::Interactive);

        // Per-column geometry vectors, seeded with the default widths as the
        // starting base (content-fit on first load and any restore override them).
        const int n = model->columnCount();
        m_baseWidth.fill(100, n);
        m_contentWidth.fill(0, n);
        m_smartMin.fill(30, n);
        m_userSet.fill(false, n);
        for (int col = 0; col < n && col < FileSystemModel::ColumnCount; ++col)
            m_baseWidth[col] = kDefaultColWidths[col];

        // Default view: hide Created and Permissions (MainWindow restores the
        // per-side hidden mask later, overriding this if the user changed it).
        header->setSectionHidden(FileSystemModel::CreatedColumn, true);
        header->setSectionHidden(FileSystemModel::PermissionsColumn, true);

        applyLayout();

        // Re-arm click-to-sort after the model is set. setSortingEnabled(true)
        // alone left the header's sectionsClickable off (the replaced
        // PlainHeaderView / DTK style doesn't get it from setSortingEnabled), so
        // header clicks never reached FileSystemModel::sort(); set it explicitly.
        // Drive sorting from the header's own sectionClicked signal instead of
        // QTableView's internal sortIndicatorChanged wiring, which the DTK style
        // / replaced PlainHeaderView leaves disconnected (header clicks never
        // reached the model). Keep the indicator shown and sections clickable.
        // The header drives sorting itself (see PlainHeaderView::mouseRelease),
        // since the DTK style never emits sectionClicked. Disable QTableView's
        // own sort wiring to avoid double handling; keep the indicator shown.
        setSortingEnabled(false);
        horizontalHeader()->setSortIndicatorShown(true);
        horizontalHeader()->setSectionsClickable(true);

        // On each directory load (the model resets), re-measure content widths
        // for the new listing and re-lay-out. User-set base widths are preserved
        // (measurement only refreshes non-user columns).
        connect(model, &QAbstractItemModel::modelReset, this,
                [this]() { recomputeContentWidths(); });
    }
}

void FileListView::sortByHeaderSection(int column) {
    // Toggle direction when re-clicking the current sort column. Decide from the
    // view's own state, not the header indicator: model()->sort() below resets the
    // model, which clears the header's indicator, so reading it back would make
    // every re-click compute "ascending" again and never toggle.
    Qt::SortOrder order = Qt::AscendingOrder;
    if (m_sortColumn == column && m_sortOrder == Qt::AscendingOrder)
        order = Qt::DescendingOrder;
    m_sortColumn = column;
    m_sortOrder = order;

    // FileSystemModel::sort() does a begin/endResetModel, which clears both the
    // selection and the current index. Remember them by path so we can restore
    // the same files after the reorder and keep the user's focused file in view
    // (centred), instead of jumping to the top with nothing selected.
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    QStringList selectedPaths;
    QString currentPath;
    if (fsModel) {
        if (QItemSelectionModel *sel = selectionModel()) {
            const QModelIndexList rows = sel->selectedRows();
            selectedPaths.reserve(rows.size());
            for (const QModelIndex &idx : rows)
                selectedPaths << fsModel->fileInfoAt(idx.row()).path();
        }
        const QModelIndex cur = currentIndex();
        if (cur.isValid())
            currentPath = fsModel->fileInfoAt(cur.row()).path();
    }

    horizontalHeader()->setSortIndicator(column, order);
    if (model())
        model()->sort(column, order);
    // The model reset above can drop the indicator; restore it so the arrow shows.
    horizontalHeader()->setSortIndicator(column, order);

    if (fsModel && (!selectedPaths.isEmpty() || !currentPath.isEmpty())) {
        // Map each surviving path back to its new row in the sorted model.
        QHash<QString, int> rowByPath;
        const int rowN = fsModel->rowCount();
        rowByPath.reserve(rowN);
        for (int r = 0; r < rowN; ++r)
            rowByPath.insert(fsModel->fileInfoAt(r).path(), r);

        QItemSelectionModel *sel = selectionModel();
        const int lastCol = fsModel->columnCount() - 1;
        if (sel) {
            QItemSelection restored;
            for (const QString &p : selectedPaths) {
                const auto it = rowByPath.constFind(p);
                if (it != rowByPath.constEnd())
                    restored.select(fsModel->index(it.value(), 0),
                                    fsModel->index(it.value(), lastCol));
            }
            sel->clearSelection();
            if (!restored.isEmpty())
                sel->select(restored, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }

        // Prefer the previously-current file as the row to reveal; fall back to
        // the first still-present selected file.
        int centerRow = -1;
        const auto findRow = [&](const QString &p) {
            const auto it = rowByPath.constFind(p);
            return it != rowByPath.constEnd() ? it.value() : -1;
        };
        if (!currentPath.isEmpty())
            centerRow = findRow(currentPath);
        if (centerRow < 0 && !selectedPaths.isEmpty())
            centerRow = findRow(selectedPaths.first());
        if (centerRow >= 0) {
            const QModelIndex cur = fsModel->index(centerRow, 0);
            if (sel)
                sel->setCurrentIndex(cur, QItemSelectionModel::NoUpdate);
            scrollTo(cur, QAbstractItemView::PositionAtCenter);
        }
    }
}

void FileListView::showColumnMenu(const QPoint &pos) {
    if (!model())
        return;
    QHeaderView *header = horizontalHeader();
    QMenu menu(this);
    for (int c = 0; c < model()->columnCount(); ++c) {
        const QString label = model()->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        QAction *action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(c));
        if (c == FileSystemModel::NameColumn)
            action->setEnabled(false); // the Name column can't be hidden
        connect(action, &QAction::toggled, this, [this, c, header](bool on) {
            header->setSectionHidden(c, !on);
            if (on) {
                // Newly shown column: fit it by the unified rule (content-min
                // width) rather than reusing a stale persisted/default width, so
                // it joins the same layout scheme as every other info column.
                // Clearing m_userSet lets recomputeContentWidths() reset its base
                // to the measured content width; other user-set columns keep
                // their widths (recompute only touches !m_userSet columns).
                m_userSet[c] = false;
                recomputeContentWidths(); // recomputes bases + applyLayout()
            } else {
                applyLayout();
            }
        });
    }
    menu.exec(header->mapToGlobal(pos));
}

void FileListView::resizeEvent(QResizeEvent *event) {
    QTableView::resizeEvent(event);
    // applyLayout() is pure arithmetic over the cached content-mins/base widths
    // (no per-row measurement), so it's cheap enough to run synchronously on
    // every interactive resize step -- no debounce or last-column-only shortcut.
    applyLayout();
}

int FileListView::measureVariableColumn(int column, const QFontMetrics &fm) const {
    // Widest DisplayRole string across ALL rows, so a long value that is
    // currently scrolled off-screen still gets a column wide enough to show it.
    // For very large listings, cap the scan -- the widest value is almost
    // certainly within the first slice, and the delegate elides anyway.
    if (!model())
        return 0;
    const int rows = model()->rowCount();
    const int scan = qMin(rows, 4000);
    int w = 0;
    for (int r = 0; r < scan; ++r) {
        const QString s = model()->index(r, column).data(Qt::DisplayRole).toString();
        if (!s.isEmpty())
            w = qMax(w, fm.horizontalAdvance(s));
    }
    return w;
}

void FileListView::recomputeContentWidths() {
    if (m_adjustingColumns || !model())
        return;
    QHeaderView *header = horizontalHeader();
    const int n = qMin(header->count(), model()->columnCount());
    if (m_baseWidth.size() != header->count()) // model column count changed
        return;

    const QFontMetrics fm = fontMetrics();
    const int kHeaderPad = 28; // paintSection insets 8 left / 20 right (arrow room)
    const int kCellPad = 16;   // delegate insets ~4/4 + breathing room

    for (int c = 0; c < n; ++c) {
        const QString headerText =
            model()->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        m_smartMin[c] = fm.horizontalAdvance(headerText) + kHeaderPad;

        int content = 0;
        switch (c) {
        case FileSystemModel::NameColumn:
            content = 0; // Name is the flex remainder; not content-measured
            break;
        case FileSystemModel::ModifiedColumn:
        case FileSystemModel::CreatedColumn:
            content = fm.horizontalAdvance(QStringLiteral("0000-00-00 00:00")) + kCellPad;
            break;
        case FileSystemModel::PermissionsColumn:
            content = fm.horizontalAdvance(QStringLiteral("drwxrwxrwx")) + kCellPad;
            break;
        default: // Ext, Size, Type -> variable
            content = measureVariableColumn(c, fm) + kCellPad;
            break;
        }

        if (c != FileSystemModel::NameColumn) {
            m_contentWidth[c] = qMax(content, m_smartMin[c]);
            if (!m_userSet[c])
                m_baseWidth[c] = m_contentWidth[c];
        }
    }
    applyLayout();
}

void FileListView::applyLayout() {
    if (m_adjustingColumns)
        return;
    QHeaderView *header = horizontalHeader();
    if (!header || header->count() == 0)
        return;
    // deepin/DTK uses OVERLAY scrollbars: the vertical bar floats over the right
    // edge of the viewport instead of shrinking it, so viewport()->width() still
    // counts the strip it covers. The bar is always-on (see the ctor), so always
    // reserve its width -- unconditionally, not via isVisible(), which can lag a
    // model reset -- so the last column's content isn't hidden under the bar.
    // Use the style's standard scrollbar thickness (PM_ScrollBarExtent, ~15-20px)
    // -- NOT verticalScrollBar()->width(), which the deepin overlay bar reports as
    // a large hit-area (~100px), over-reserving and crushing the columns.
    const int avail =
        viewport()->width() - qBound(12, style()->pixelMetric(QStyle::PM_ScrollBarExtent), 24);
    if (avail <= 0) // pre-show / zero width: defer to the next resize/reset
        return;
    if (m_baseWidth.size() != header->count())
        return;

    const QFontMetrics fm = fontMetrics();
    // "16 characters" floor for the Name column: the width of 16 ASCII chars.
    // The row icon is deliberately NOT added on top -- doing so over-reserved the
    // Name column and crowded out the info columns, so a long "APPIMAGE" type
    // couldn't get its content width. averageCharWidth() is avoided (a CJK font
    // inflates it by averaging in full-width glyphs).
    m_nameFloor = fm.horizontalAdvance(QStringLiteral("0000000000000000"));

    // Visible columns in logical order (Name is index 0 and never hidden).
    QVector<int> visible;
    for (int c = 0; c < header->count(); ++c)
        if (!header->isSectionHidden(c))
            visible.append(c);
    if (visible.isEmpty())
        return;

    // Tentative: info columns at their base, Name absorbs the remainder.
    QVector<int> disp = m_baseWidth;
    int sumInfoBase = 0;
    for (int c : visible)
        if (c != FileSystemModel::NameColumn)
            sumInfoBase += m_baseWidth[c];

    const int nameW = avail - sumInfoBase;
    if (nameW < m_nameFloor) {
        // Shrink phase: Name is pinned to its floor, so compress info columns by
        // priority (least important first) down to their smart-min.
        int deficit = sumInfoBase - (avail - m_nameFloor);
        static const int kCompressOrder[] = {
            FileSystemModel::CreatedColumn, FileSystemModel::PermissionsColumn,
            FileSystemModel::ExtColumn,     FileSystemModel::TypeColumn,
            FileSystemModel::ModifiedColumn, FileSystemModel::SizeColumn};
        for (int c : kCompressOrder) {
            if (deficit <= 0)
                break;
            if (c >= disp.size() || header->isSectionHidden(c))
                continue;
            const int room = disp[c] - m_smartMin[c];
            const int take = qMin(qMax(0, room), deficit);
            disp[c] -= take;
            deficit -= take;
        }
    }

    // Name is the SOLE flex column: it absorbs the exact remainder after every
    // info column (including the LAST) keeps its own width. That way each info
    // column shows its content and double-click auto-fit can actually widen the
    // last (Type) column -- unlike a "last column takes the remainder" scheme,
    // where the last column's own width would be ignored. Integer math makes the
    // total exactly `avail`, so the last column's right edge stays pinned.
    int sumInfoDisp = 0;
    for (int c : visible)
        if (c != FileSystemModel::NameColumn)
            sumInfoDisp += disp[c];
    if (FileSystemModel::NameColumn < disp.size())
        disp[FileSystemModel::NameColumn] =
            qMax(m_smartMin[FileSystemModel::NameColumn], avail - sumInfoDisp);

    m_adjustingColumns = true;
    for (int c = 0; c < header->count(); ++c) {
        if (header->isSectionHidden(c)) {
            // resizeSection is a no-op on a hidden section, so a hidden column
            // keeps a phantom slot in the layout (reports size 0 yet still
            // offsets later columns, leaving a blank gap before the next visible
            // column and pushing the last column off-screen). Briefly show it to
            // collapse its width to 0 (minimumSectionSize is 0), then re-hide.
            header->showSection(c);
            header->resizeSection(c, 0);
            header->hideSection(c);
        } else {
            header->resizeSection(c, disp.value(c));
        }
    }
    m_adjustingColumns = false;
}

void FileListView::onSectionResized(int logical, int oldSize, int newSize) {
    if (m_adjustingColumns) // our own applyLayout() resizeSection -> ignore
        return;
    QHeaderView *header = horizontalHeader();
    if (logical < 0 || logical >= m_baseWidth.size())
        return;

    // Find the next visible column after `logical`.
    int next = -1;
    for (int c = logical + 1; c < header->count(); ++c) {
        if (!header->isSectionHidden(c)) {
            next = c;
            break;
        }
    }
    if (next < 0) {
        // Dragged the last visible column's right border -> locked. Revert.
        applyLayout();
        return;
    }

    const int delta = newSize - oldSize;
    if (logical != FileSystemModel::NameColumn) {
        m_baseWidth[logical] = qMax(m_smartMin[logical], newSize);
        m_userSet[logical] = true;
    }
    // Trade the delta out of the next visible column (adjacent give-and-take).
    m_baseWidth[next] = qMax(m_smartMin[next], m_baseWidth[next] - delta);
    m_userSet[next] = true;
    applyLayout(); // re-pin the last column and keep the sum = viewport
}

void FileListView::autoFitColumnRightOfHandle(int handleLeftLogical) {
    // Qt reports the divider by the column to its LEFT; the user means the column
    // to its RIGHT. Find the next visible column and fit that one. The rightmost
    // divider (last visible column's right edge, pinned to the viewport) has no
    // column to its right, so nothing happens there.
    QHeaderView *header = horizontalHeader();
    if (!header)
        return;
    for (int c = handleLeftLogical + 1; c < header->count(); ++c) {
        if (!header->isSectionHidden(c)) {
            autoFitColumn(c);
            return;
        }
    }
}

void FileListView::autoFitColumn(int logical) {
    // Fit an info column to its (freshly measured) content width: exactly the
    // widest value it must display (the header label is folded into m_contentWidth
    // as a floor, so a short column still shows its title). Only this column's
    // base changes; applyLayout then makes the Name column absorb the difference,
    // leaving every other info column untouched -- unless Name would fall below
    // its 16-char floor, which triggers the usual priority compression. The Name
    // column itself is the flex remainder, so there is nothing to "fit" for it.
    if (logical <= FileSystemModel::NameColumn || logical >= m_baseWidth.size() || !model())
        return;
    recomputeContentWidths(); // refresh m_contentWidth for the current listing
    m_baseWidth[logical] = qMax(m_contentWidth.value(logical), m_smartMin.value(logical));
    m_userSet[logical] = true;
    applyLayout();
}

void FileListView::restoreColumnLayout(const QVector<int> &baseWidths, int hiddenMask, int sortCol,
                                       Qt::SortOrder sortOrder) {
    QHeaderView *header = horizontalHeader();
    // Guard the whole restore: setSectionHidden / any resize below must not be
    // mistaken for a user drag (onSectionResized would otherwise re-adopt them).
    m_adjustingColumns = true;
    if (hiddenMask >= 0)
        for (int c = 0; c < header->count(); ++c)
            header->setSectionHidden(c, (hiddenMask & (1 << c)) != 0);
    const int n = qMin(baseWidths.size(), m_baseWidth.size());
    for (int c = 0; c < n; ++c) {
        if (c == FileSystemModel::NameColumn)
            continue; // Name is the flex remainder; its stored slot is ignored
        if (baseWidths.at(c) > 0) {
            m_baseWidth[c] = baseWidths.at(c);
            m_userSet[c] = true;
        }
    }
    if (sortCol >= 0) {
        m_sortColumn = sortCol;
        m_sortOrder = sortOrder;
    }
    m_adjustingColumns = false;

    if (sortCol >= 0 && model())
        model()->sort(sortCol, sortOrder); // resets model -> recompute + applyLayout
    else
        applyLayout();
    header->setSortIndicator(qMax(0, m_sortColumn), m_sortOrder);
}

void FileListView::setSort(int column, Qt::SortOrder order) {
    m_sortColumn = column;
    m_sortOrder = order;
    horizontalHeader()->setSortIndicator(column, order);
    if (model())
        model()->sort(column, order); // resets model -> recomputeContentWidths + applyLayout
    horizontalHeader()->setSortIndicator(column, order);
}

void FileListView::ensureSelectionPalettes() {
    if (m_selectionPalettesValid)
        return;

    // Derive both palettes once from the theme QSS: temporarily toggle the
    // panelActive property and let QStyleSheetStyle fold its selection-colour
    // rules into the palette. QStyleSheetStyle::polish() also re-resolves the
    // widget font from the global stylesheet, dropping any size set via
    // setFont(), so preserve and restore it around the probing.
    const QFont keep = font();
    const QVariant prev = property("panelActive");

    // Clear any previously-applied explicit palette first: an explicitly-set
    // role stops QStyleSheetStyle from folding the theme's colour into it on
    // polish, which would otherwise make us re-capture stale colours after a
    // theme switch.
    setPalette(QPalette());

    setProperty("panelActive", true);
    style()->unpolish(this);
    style()->polish(this);
    m_activePalette = palette();

    setProperty("panelActive", false);
    style()->unpolish(this);
    style()->polish(this);
    m_inactivePalette = palette();

    setProperty("panelActive", prev);
    if (font() != keep)
        setFont(keep);
    m_selectionPalettesValid = true;
}

void FileListView::setPanelActive(bool active) {
    if (m_panelActiveKnown && m_panelActive == active)
        return;
    m_panelActive = active;
    m_panelActiveKnown = true;

    // Fast path: swap in the pre-derived palette instead of a full repolish.
    // The delegate paints selection from palette Highlight/HighlightedText, so a
    // cheap setPalette() is enough -- no QStyleSheetStyle re-resolution (and no
    // font-reset side effect) on every panel switch. Keep the dynamic property
    // in sync so any later repolish still yields the same colours.
    ensureSelectionPalettes();
    setProperty("panelActive", active);
    setPalette(active ? m_activePalette : m_inactivePalette);
    viewport()->update();
}

void FileListView::changeEvent(QEvent *event) {
    if (event->type() == QEvent::StyleChange) {
        // The application stylesheet (theme) changed: the cached palettes are
        // stale. Drop them and re-apply the current state's freshly-derived
        // colours so the selection tint never lags a theme switch.
        m_selectionPalettesValid = false;
        if (m_panelActiveKnown) {
            ensureSelectionPalettes();
            setPalette(m_panelActive ? m_activePalette : m_inactivePalette);
        }
    } else if (event->type() == QEvent::FontChange) {
        // The list font drives content-width measurement; a font change (the
        // panel applies the configured list font after construction, or the user
        // changes the View-menu font size) invalidates the cached widths.
        recomputeContentWidths();
    }
    QTableView::changeEvent(event);
}

void FileListView::keyboardSearch(const QString &search) {
    // Qt's default matches against the current index's column; force column 0
    // (Name) so typing always jumps by file name even after clicking a Size
    // or Date cell.
    const QModelIndex cur = currentIndex();
    if (cur.isValid() && cur.column() != 0)
        setCurrentIndex(cur.sibling(cur.row(), 0));
    QTableView::keyboardSearch(search);
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

void FileListView::mousePressEvent(QMouseEvent *event) {
    m_renameClickTimer->stop();
    m_renameClickIndex = QModelIndex();

    // Remember whether this press landed on the name/ext cell of a row that was
    // already the sole selection -- that's the signal for "rename in place".
    // Evaluate before the base class mutates the selection.
    if (event->button() == Qt::LeftButton
        && !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
        && state() != QAbstractItemView::EditingState && model() && selectionModel()) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid() && (idx.column() == FileSystemModel::NameColumn
                              || idx.column() == FileSystemModel::ExtColumn)
            && (model()->flags(idx) & Qt::ItemIsEditable)
            && selectionModel()->isRowSelected(idx.row(), QModelIndex())
            && selectionModel()->selectedRows().size() == 1)
            m_renameClickIndex = idx;
    }
    QTableView::mousePressEvent(event);
}

void FileListView::mouseReleaseEvent(QMouseEvent *event) {
    QTableView::mouseReleaseEvent(event);
    // Start the rename only if the button came up on the same cell it went
    // down on; defer by the double-click interval so a double-click (open)
    // cancels it first (see mouseDoubleClickEvent).
    if (m_renameClickIndex.isValid()
        && indexAt(event->pos()) == QModelIndex(m_renameClickIndex))
        m_renameClickTimer->start(QApplication::doubleClickInterval() + 10);
    else
        m_renameClickIndex = QModelIndex();
}

void FileListView::mouseDoubleClickEvent(QMouseEvent *event) {
    // A double-click opens the file -- never renames.
    m_renameClickTimer->stop();
    m_renameClickIndex = QModelIndex();
    QTableView::mouseDoubleClickEvent(event);
}

void FileListView::startDrag(Qt::DropActions supportedActions) {
    m_renameClickTimer->stop();
    m_renameClickIndex = QModelIndex();

    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return;

    QList<QUrl> urls;
    QModelIndex firstIdx;
    for (const QModelIndex &idx : selectionModel()->selectedRows()) {
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

void FileListView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FileListView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FileListView::dragLeaveEvent(QDragLeaveEvent *event) {
    // We draw no drop indicator, so skip QAbstractItemView's dragLeave handler,
    // which repaints the entire viewport. That full repaint fired every time a
    // fast drag crossed out of this view (e.g. over the splitter into the other
    // panel), causing a visible stutter. Accepting without a repaint is enough.
    event->accept();
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
