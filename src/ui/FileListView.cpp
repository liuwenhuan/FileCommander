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
#include <QPointer>
#include <QPolygon>
#include <QStyledItemDelegate>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QVector>
#include <QWheelEvent>
#include <QWidget>

#include "DragPixmap.h"
#include "ExternalPaths.h"
#include "FileSystemModel.h"
#include "MotionPolicy.h"

namespace {
// Baseline column proportions (Name, Ext, Size, Modified, Type, Created,
// Permissions). Used as the initial layout and as a per-column floor for the
// content-aware auto-fit so no column ever collapses.
constexpr int kDefaultColWidths[FileSystemModel::ColumnCount] = {280, 70, 100, 150,
                                                                 90,  150, 110};
constexpr int kDragFeedbackEnterDurationMs = 80;
constexpr int kDragFeedbackSuccessDurationMs = 150;

int textInkWidth(const QFontMetrics &fm, const QString &text) {
    if (text.isEmpty())
        return 0;
    // horizontalAdvance() is layout advance, not a strict ink bounding box. Some
    // fonts draw a few pixels outside the advance; right-aligned columns then
    // look clipped even after auto-fit. Use the larger value for column sizing.
    return qMax(fm.horizontalAdvance(text), fm.boundingRect(text).width());
}

// Header that paints its own (non-bold) section labels. The deepin (DTK)
// style draws header text bold and ignores the widget font / qss font-weight,
// so we bypass it here, matching the theme's section colours.
//
// Because the painting is ours, `QHeaderView::section { background-color: ... }`
// in a stylesheet never reaches it -- the section colours below are picked from
// the window lightness instead. That is fine for a theme that is simply light or
// dark, but not for one with a hue of its own (the CRT theme's near-black green
// would read as "dark" and get a grey header). The three qproperty hooks let a
// stylesheet name the colours outright:
//
//     QHeaderView { qproperty-sectionBackground: #0d1a0d; ... }
//
// Each defaults to an invalid QColor, which means "not themed" and keeps the
// original light/dark choice, so light.qss and dark.qss need no changes.
class PlainHeaderView : public QHeaderView {
    Q_OBJECT
    Q_PROPERTY(QColor sectionBackground MEMBER m_sectionBg)
    Q_PROPERTY(QColor sectionForeground MEMBER m_sectionFg)
    Q_PROPERTY(QColor sectionBorder MEMBER m_sectionBorder)

public:
    explicit PlainHeaderView(QWidget *parent = nullptr) : QHeaderView(Qt::Horizontal, parent) {}

signals:
    void resizeHandlePressed(int leftLogical);

protected:
    void mousePressEvent(QMouseEvent *e) override {
        m_pressIndex = logicalIndexAt(e->pos().x());
        m_pressPos = e->pos();
        if (e->button() == Qt::LeftButton) {
            const int handleLeft = resizeGripLeftSection(e->pos().x());
            if (handleLeft >= 0)
                emit resizeHandlePressed(handleLeft);
        }
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
            (e->pos() - m_pressPos).manhattanLength() < 4 &&
            resizeGripLeftSection(m_pressPos.x()) < 0) {
            if (auto *view = qobject_cast<FileListView *>(parentWidget()))
                view->sortByHeaderSection(m_pressIndex);
        }
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        // Emit the handle signal ourselves for resize-grip double-clicks. The
        // base implementation also performs a default section resize first,
        // which looks like a user drag to FileListView and makes the adjacent
        // column give up width before our content-fit rule runs.
        const int handleLeft = resizeGripLeftSection(e->pos().x());
        if (handleLeft >= 0) {
            emit sectionHandleDoubleClicked(handleLeft);
        } else {
            QHeaderView::mouseDoubleClickEvent(e);
        }
        // A double-click -- whether it auto-fits a column or lands on a label --
        // must not sort: clear the press index so the trailing release doesn't.
        m_pressIndex = -1;
    }
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override {
        if (!rect.isValid() || !model())
            return;
        const bool light = palette().color(QPalette::Window).lightness() > 128;
        const QColor bg =
            m_sectionBg.isValid() ? m_sectionBg
                                  : (light ? QColor(0xec, 0xec, 0xec) : QColor(0x23, 0x23, 0x23));
        const QColor fg =
            m_sectionFg.isValid() ? m_sectionFg
                                  : (light ? QColor(0x20, 0x20, 0x20) : QColor(0xe0, 0xe0, 0xe0));
        // Divider between header sections. In dark mode 0x1a was nearly
        // indistinguishable from the 0x23 header background, so the column
        // separators vanished; use a mid grey that reads clearly on both.
        const QColor border = m_sectionBorder.isValid()
                                  ? m_sectionBorder
                                  : (light ? QColor(0xd0, 0xd0, 0xd0) : QColor(0x50, 0x50, 0x50));

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
    int resizeGripLeftSection(int x) const {
        const int grip =
            qMax(5, style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this));
        for (int i = 0; i < count(); ++i) {
            if (isSectionHidden(i))
                continue;
            const int right = sectionViewportPosition(i) + sectionSize(i);
            if (qAbs(x - right) <= grip)
                return i;
        }
        return -1;
    }

    int m_pressIndex = -1;
    QPoint m_pressPos;
    // Invalid until a stylesheet sets them -- see the class note.
    QColor m_sectionBg;
    QColor m_sectionFg;
    QColor m_sectionBorder;
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
    explicit FileItemDelegate(QTableView *view)
        : QStyledItemDelegate(view), m_view(view) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index); // fills text, icon, font, displayAlignment

        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette &pal = opt.palette;

        painter->save();
        painter->setClipRect(opt.rect);

        // Selected rows stay opaque and high contrast. Ordinary rows leave the
        // viewport surface visible; in the CRT theme that surface owns one
        // stable scanline tile, while light/dark still provide their solid base.
        if (selected)
            painter->fillRect(opt.rect, pal.highlight());

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

        drawCursorFrame(painter, opt, index, selected);
        painter->restore();
    }

private:
    // The keyboard cursor. Arrow keys move the current index without touching
    // the selection (see FileListView::keyPressEvent), so on an unselected row
    // this frame is the ONLY thing that moves -- without it the arrow keys read
    // as dead. Drawn from the same palette entry as the selection, so an
    // inactive panel dims it exactly like its selection (QTableView
    // [panelActive="false"] in the themes).
    //
    // Every pixel of it stays inside opt.rect. That is not a style preference,
    // it is the contract Qt's incremental repainting is built on: the view
    // marks dirty work in units of cells, so a delegate that paints outside its
    // own cell paints something nobody will ever be told to erase. An earlier
    // version drew one rectangle across the whole row from a single cell -- it
    // had no seams, and it left a rule behind on every row the cursor passed
    // through, because leaving a row only ever repainted that row's cells.
    //
    // So the frame is assembled from per-cell pieces after all. The seams they
    // have to meet at are exact: each piece spans its cell's FULL width,
    // left() to right() inclusive, and cells tile edge to edge. Taking one
    // pixel off the right (QRect::right() is already the last pixel inside the
    // rect) is what put a hole at every column boundary once before.
    void drawCursorFrame(QPainter *painter, const QStyleOptionViewItem &opt,
                         const QModelIndex &index, bool selected) const {
        if (!m_view)
            return;
        const QModelIndex current = m_view->currentIndex();
        if (!current.isValid() || current.row() != index.row())
            return;

        const QPalette &pal = opt.palette;
        const QColor colour =
            selected ? pal.highlightedText().color() : pal.highlight().color();

        // fillRect, not drawLine. A one-pixel stroke is a path, and on a
        // fractionally scaled display its coordinates land between device
        // pixels: at 125% a 21px row is 26.25 device pixels, so row boundaries
        // are whole numbers only every fourth row and the rules on the rows in
        // between rasterised to nothing. Filling a 1px-tall rectangle asks for
        // an area instead of a path, and an area always covers the pixels it
        // overlaps. This is what "every fourth row has no frame" was.
        const QRect r = opt.rect;
        painter->fillRect(QRect(r.left(), r.top(), r.width(), 1), colour);
        painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), colour);

        // The ends belong only to the outermost visible columns.
        QHeaderView *header = m_view->horizontalHeader();
        if (!header)
            return;
        int firstVisual = -1;
        int lastVisual = -1;
        for (int visual = 0; visual < header->count(); ++visual) {
            if (header->isSectionHidden(header->logicalIndex(visual)))
                continue;
            if (firstVisual < 0)
                firstVisual = visual;
            lastVisual = visual;
        }
        const int visual = header->visualIndex(index.column());
        if (visual == firstVisual)
            painter->fillRect(QRect(r.left(), r.top(), 1, r.height()), colour);
        if (visual == lastVisual)
            painter->fillRect(QRect(r.right(), r.top(), 1, r.height()), colour);
    }

    QPointer<QTableView> m_view;

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
    m_verticalScrollBarContainer = verticalScrollBar()->parentWidget();
    if (m_verticalScrollBarContainer)
        m_verticalScrollBarContainer->installEventFilter(this);
    // Columns are always managed to fill the viewport exactly (applyLayout), so
    // a horizontal scrollbar is never wanted -- turn it off so the last column's
    // right edge stays pinned (Qt otherwise flashes the bar at the exact-fit
    // boundary). In the extreme-narrow case the last column simply clips.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalHeader()->hide();
    auto *plainHeader = new PlainHeaderView(this);
    setHorizontalHeader(plainHeader); // non-bold, self-painted labels
    connect(plainHeader, &PlainHeaderView::resizeHandlePressed, this,
            [this, plainHeader](int) {
                if (!m_preHandleResizeBaseWidth.isEmpty())
                    return;
                m_preHandleResizeBaseWidth.resize(plainHeader->count());
                for (int column = 0; column < plainHeader->count(); ++column)
                    m_preHandleResizeBaseWidth[column] = plainHeader->sectionSize(column);
                QTimer::singleShot(QApplication::doubleClickInterval() + 50, this, [this] {
                    m_preHandleResizeBaseWidth.clear();
                });
            });
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

    m_dragFeedbackAnimation = new QVariantAnimation(this);
    m_dragFeedbackAnimation->setObjectName(QStringLiteral("DragTargetFeedbackAnimation"));
    connect(m_dragFeedbackAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { setDragFeedbackColor(value.value<QColor>()); });
    connect(m_dragFeedbackAnimation, &QVariantAnimation::finished, this, [this] {
        if (m_dragFeedbackState == DragFeedbackState::Success)
            clearDragFeedback();
    });

    m_dragFeedbackClearTimer = new QTimer(this);
    m_dragFeedbackClearTimer->setSingleShot(true);
    m_dragFeedbackClearTimer->setInterval(kDragFeedbackSuccessDurationMs);
    connect(m_dragFeedbackClearTimer, &QTimer::timeout, this, &FileListView::clearDragFeedback);

    MotionPolicy::observeReduced(this, [this](bool reduced) {
        if (reduced && m_dragFeedbackState != DragFeedbackState::None)
            showDragFeedback(m_dragFeedbackState, 0);
    });
}

QString FileListView::dragFeedbackState() const {
    switch (m_dragFeedbackState) {
    case DragFeedbackState::Accepted:
        return QStringLiteral("accepted");
    case DragFeedbackState::Rejected:
        return QStringLiteral("rejected");
    case DragFeedbackState::Success:
        return QStringLiteral("success");
    case DragFeedbackState::None:
        return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

void FileListView::setModel(QAbstractItemModel *model) {
    clearDragFeedback();
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
        m_lastStableBaseWidth = m_baseWidth;

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
        connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            recomputeContentWidths();
            clearDragFeedback();
        });
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
    placeVerticalScrollBarBelowHeader();
    scheduleVerticalScrollBarPlacement();
    // applyLayout() is pure arithmetic over the cached content-mins/base widths
    // (no per-row measurement), so it's cheap enough to run synchronously on
    // every interactive resize step -- no debounce or last-column-only shortcut.
    applyLayout();
}

void FileListView::updateGeometries() {
    QTableView::updateGeometries();
    placeVerticalScrollBarBelowHeader();
    scheduleVerticalScrollBarPlacement();
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
            w = qMax(w, textInkWidth(fm, s));
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
    const int kCellPad = 24;   // delegate insets 4/4 + room for right-aligned ink near dividers

    for (int c = 0; c < n; ++c) {
        const QString headerText =
            model()->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        m_smartMin[c] = textInkWidth(fm, headerText) + kHeaderPad;

        int content = 0;
        switch (c) {
        case FileSystemModel::NameColumn:
            content = 0; // Name is the flex remainder; not content-measured
            break;
        case FileSystemModel::ModifiedColumn:
        case FileSystemModel::CreatedColumn:
            content = textInkWidth(fm, QStringLiteral("0000-00-00 00:00")) + kCellPad;
            break;
        case FileSystemModel::PermissionsColumn:
            content = textInkWidth(fm, QStringLiteral("drwxrwxrwx")) + kCellPad;
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

void FileListView::applyLayout(int protectedLeftColumn, int protectedTargetColumn) {
    if (m_adjustingColumns)
        return;
    QHeaderView *header = horizontalHeader();
    if (!header || header->count() == 0)
        return;
    const int avail = columnLayoutWidth();
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
    static const int kCompressOrder[] = {
        FileSystemModel::CreatedColumn, FileSystemModel::PermissionsColumn,
        FileSystemModel::ExtColumn,     FileSystemModel::TypeColumn,
        FileSystemModel::ModifiedColumn, FileSystemModel::SizeColumn};
    if (nameW < m_nameFloor) {
        // Shrink phase: Name is pinned to its floor, so compress info columns by
        // priority (least important first) down to their smart-min.
        int deficit = sumInfoBase - (avail - m_nameFloor);
        for (int c : kCompressOrder) {
            if (deficit <= 0)
                break;
            if (c >= disp.size() || header->isSectionHidden(c))
                continue;
            if (c == protectedLeftColumn || c == protectedTargetColumn)
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
    // where the last column's own width would be ignored.
    int sumInfoDisp = 0;
    for (int c : visible)
        if (c != FileSystemModel::NameColumn)
            sumInfoDisp += disp[c];

    // Smart minima preserve readable headers whenever the viewport can afford
    // them. Below that point, keep the table's geometry exact rather than letting
    // the sections overflow: reduce the least-important info columns to zero in
    // the same priority order. The sections remain visible (not hidden), so their
    // order and drag behavior are unchanged; a narrow panel merely clips the
    // controls that have no width to paint.
    const bool protectsAutoFitColumns = protectedLeftColumn >= 0 || protectedTargetColumn >= 0;
    const int infoBudget = qMax(0, avail - (protectsAutoFitColumns ? 0 : m_nameFloor));
    int emergencyDeficit = sumInfoDisp - infoBudget;
    if (emergencyDeficit > 0) {
        for (int c : kCompressOrder) {
            if (emergencyDeficit <= 0)
                break;
            if (c >= disp.size() || header->isSectionHidden(c))
                continue;
            if (c == protectedLeftColumn || c == protectedTargetColumn)
                continue;
            const int take = qMin(disp[c], emergencyDeficit);
            disp[c] -= take;
            emergencyDeficit -= take;
        }
        sumInfoDisp = 0;
        for (int c : visible)
            if (c != FileSystemModel::NameColumn)
                sumInfoDisp += disp[c];
    }
    // In an impossibly narrow viewport the two protected columns alone may not
    // fit. Keep the divider's left column stable for as long as possible: first
    // trim the auto-fit target, and only then the left column as a final fallback.
    int protectedOverflow = sumInfoDisp - avail;
    for (int c : {protectedTargetColumn, protectedLeftColumn}) {
        if (protectedOverflow <= 0 || c < 0 || c >= disp.size())
            continue;
        const int take = qMin(disp[c], protectedOverflow);
        disp[c] -= take;
        sumInfoDisp -= take;
        protectedOverflow -= take;
    }
    if (FileSystemModel::NameColumn < disp.size())
        disp[FileSystemModel::NameColumn] = avail - sumInfoDisp;

    m_adjustingColumns = true;
    if (m_expectedProgrammaticWidth.size() != header->count())
        m_expectedProgrammaticWidth.fill(-1, header->count());
    for (int c = 0; c < header->count(); ++c) {
        if (header->isSectionHidden(c)) {
            // resizeSection is a no-op on a hidden section, so a hidden column
            // keeps a phantom slot in the layout (reports size 0 yet still
            // offsets later columns, leaving a blank gap before the next visible
            // column and pushing the last column off-screen). Briefly show it to
            // collapse its width to 0 (minimumSectionSize is 0), then re-hide.
            m_expectedProgrammaticWidth[c] = 0;
            header->showSection(c);
            header->resizeSection(c, 0);
            header->hideSection(c);
        } else {
            const int desired = disp.value(c);
            if (header->sectionSize(c) == desired) {
                m_expectedProgrammaticWidth[c] = -1;
            } else {
                m_expectedProgrammaticWidth[c] = desired;
                header->resizeSection(c, desired);
            }
        }
    }
    m_adjustingColumns = false;
}

int FileListView::columnLayoutWidth() const {
    // The vertical scrollbar starts below the column header, so its gutter is
    // part of the header's usable geometry. Filling the whole panel makes the
    // final header section meet the splitter (or window edge) instead of leaving
    // an empty strip above the scrollbar.
    return contentsRect().width();
}

void FileListView::placeVerticalScrollBarBelowHeader() {
    QScrollBar *bar = verticalScrollBar();
    if (!bar || !bar->isVisible())
        return;

    QHeaderView *header = horizontalHeader();
    const QRect content = contentsRect();
    if (header && header->isVisible()) {
        const QRect geometry = header->geometry();
        // QAbstractScrollArea normally shortens the header to reserve the full
        // scrollbar gutter. Our scrollbar begins below it, so the header must
        // own that top-right area and end at the panel boundary.
        header->setGeometry(content.left(), geometry.top(), content.width(), geometry.height());
    }
    const int top = header && header->isVisible()
                        ? header->geometry().bottom() + 1
                        : viewport()->geometry().top();
    const int width = qMax(1, bar->sizeHint().width());
    const int left = content.right() - width + 1;
    const int bottom = content.bottom();
    if (bottom < top)
        return;
    const int height = bottom - top + 1;
    QWidget *barContainer = bar->parentWidget();
    if (barContainer && barContainer != this) {
        // QAbstractScrollArea owns the scrollbar through
        // qt_scrollarea_vcontainer. Positioning the scrollbar with view
        // coordinates is ineffective because the container's layout resets it
        // to (0, 0) after a style change. Move the container in view coordinates
        // and let the scrollbar fill its local coordinate space instead.
        const QRect containerGeometry(left, top, width, height);
        const QRect barGeometry(0, 0, width, height);
        if (barContainer->geometry() != containerGeometry)
            barContainer->setGeometry(containerGeometry);
        if (bar->geometry() != barGeometry)
            bar->setGeometry(barGeometry);
        barContainer->raise();
    } else {
        bar->setGeometry(left, top, width, height);
        bar->raise();
    }
    // Keep the header above the scrollbar if Qt schedules a late geometry pass.
    // The bar's own geometry still starts immediately below the header.
    if (header && header->isVisible())
        header->raise();
}

void FileListView::scheduleVerticalScrollBarPlacement() {
    if (m_scrollbarPlacementPending)
        return;
    m_scrollbarPlacementPending = true;
    QTimer::singleShot(0, this, [this] {
        m_scrollbarPlacementPending = false;
        placeVerticalScrollBarBelowHeader();
    });
}

bool FileListView::eventFilter(QObject *watched, QEvent *event) {
    const bool filtered = QTableView::eventFilter(watched, event);
    if (watched == m_verticalScrollBarContainer
        && (event->type() == QEvent::Move || event->type() == QEvent::Resize
            || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest)) {
        scheduleVerticalScrollBarPlacement();
    }
    return filtered;
}

void FileListView::onSectionResized(int logical, int oldSize, int newSize) {
    if (logical < 0 || logical >= m_baseWidth.size())
        return;
    if (logical < m_expectedProgrammaticWidth.size() &&
        m_expectedProgrammaticWidth.at(logical) >= 0) {
        if (m_expectedProgrammaticWidth.at(logical) == newSize)
            m_expectedProgrammaticWidth[logical] = -1;
        return;
    }
    if (m_adjustingColumns) // our own applyLayout() resizeSection -> ignore
        return;
    QHeaderView *header = horizontalHeader();
    if (!(QApplication::mouseButtons() & Qt::LeftButton))
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
    // Qt reports the divider by the column to its LEFT. FileCommander's
    // double-click rule fits the visible column on the RIGHT of that divider,
    // matching the user's target column while leaving the left column alone.
    QHeaderView *header = horizontalHeader();
    if (!header || handleLeftLogical < 0 || handleLeftLogical >= header->count())
        return;
    const int leftWidthBeforeFit =
        m_preHandleResizeBaseWidth.size() == m_baseWidth.size()
            ? m_preHandleResizeBaseWidth.value(handleLeftLogical)
            : m_lastStableBaseWidth.value(
                  handleLeftLogical,
                  m_baseWidth.value(handleLeftLogical, header->sectionSize(handleLeftLogical)));
    m_preHandleResizeBaseWidth.clear();
    for (int c = handleLeftLogical + 1; c < header->count(); ++c) {
        if (!header->isSectionHidden(c)) {
            recomputeContentWidths();
            m_baseWidth[handleLeftLogical] = leftWidthBeforeFit;
            m_userSet[handleLeftLogical] = true;
            m_baseWidth[c] = qMax(m_contentWidth.value(c), m_smartMin.value(c));
            m_userSet[c] = true;
            applyLayout(handleLeftLogical, c);
            m_lastStableBaseWidth = m_baseWidth;
            return;
        }
    }
    applyLayout();
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
    m_lastStableBaseWidth = m_baseWidth;

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
        scheduleVerticalScrollBarPlacement();
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
    // Insert alongside Space: Total Commander's primary multi-select key, with
    // exactly the same "toggle this row, then step down" behaviour. Space adds
    // one thing Insert does not -- see unselectedRowSpaced().
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Insert) {
        const QModelIndex idx = currentIndex();
        if (idx.isValid() && selectionModel()) {
            if (event->key() == Qt::Key_Space)
                emit rowSpaced(idx.row());
            selectionModel()->select(idx, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
            const QModelIndex next = idx.sibling(idx.row() + 1, idx.column());
            if (next.isValid()) {
                selectionModel()->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
                scrollTo(next, QAbstractItemView::EnsureVisible);
            }
        }
        event->accept();
        return;
    }

    // Plain cursor movement must leave the selection alone. This view is
    // ExtendedSelection, where the base class selects whatever the cursor lands
    // on, so arrowing through the list silently rewrote the user's selection --
    // and left Space with nothing to switch ON, because the row under the cursor
    // was already selected by the act of moving there. Total Commander keeps the
    // cursor and the selection independent; this brings the keyboard half of
    // that across, deliberately without changing what a mouse click does.
    //
    // Shift (extend the range) and Ctrl (move without selecting) already mean
    // something here, so both are left to the base class.
    if (event->modifiers() == Qt::NoModifier && selectionModel()) {
        CursorAction action = MoveDown;
        bool navigating = true;
        switch (event->key()) {
        case Qt::Key_Up: action = MoveUp; break;
        case Qt::Key_Down: action = MoveDown; break;
        case Qt::Key_PageUp: action = MovePageUp; break;
        case Qt::Key_PageDown: action = MovePageDown; break;
        case Qt::Key_Home: action = MoveHome; break;
        case Qt::Key_End: action = MoveEnd; break;
        default: navigating = false; break;
        }
        if (navigating) {
            const QModelIndex target = moveCursor(action, Qt::NoModifier);
            if (target.isValid()) {
                selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
                scrollTo(target, QAbstractItemView::EnsureVisible);
            }
            event->accept();
            return;
        }
    }

    QTableView::keyPressEvent(event);
}

void FileListView::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QTableView::currentChanged(current, previous);
    // The base class repaints visualRect() of each index, which under
    // SelectRows is one cell -- so the row the cursor just left would keep the
    // rest of its frame on screen. Repaint both rows edge to edge.
    auto repaintRow = [this](const QModelIndex &index) {
        if (!index.isValid())
            return;
        const QRect cell = visualRect(index);
        if (!cell.isEmpty())
            viewport()->update(0, cell.y(), viewport()->width(), cell.height());
    };
    repaintRow(previous);
    repaintRow(current);
}

void FileListView::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    if (delta == 0)
        delta = event->pixelDelta().y();
    if ((event->modifiers() & Qt::ControlModifier) && delta != 0) {
        emit zoomRequested(delta > 0 ? 1 : -1);
        event->accept();
        return;
    }
    QTableView::wheelEvent(event);
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

    QStringList paths;
    QModelIndex firstIdx;
    for (const QModelIndex &idx : selectionModel()->selectedRows()) {
        if (fsModel->isParentEntry(idx.row()))
            continue;
        // The drag icon shows the topmost (first-listed) selected item.
        if (!firstIdx.isValid() || idx.row() < firstIdx.row())
            firstIdx = idx;
        paths.append(fsModel->fileInfoAt(idx.row()).path());
    }
    if (paths.isEmpty())
        return;

    auto *mimeData = new QMimeData;
    // The private format carries the backend's own paths and is what an in-app
    // drop reads. The public URL list is built separately and may legitimately
    // come out empty: these paths belong to a server or to an archive, and a
    // file:// URL over one of them would make the receiving application open a
    // same-named LOCAL file (see fc::externalUrlsFor).
    fc::setPathPayload(mimeData, fsModel->provider(), paths, /*cut=*/false);

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    // Show what's actually being dragged: the first item's icon, plus a stacked
    // pile + count badge for a multi-item drag.
    const QIcon icon =
        fsModel->index(firstIdx.row(), FileSystemModel::NameColumn).data(Qt::DecorationRole).value<QIcon>();
    drag->setPixmap(ttc::makeDragPixmap(icon, paths.size(), devicePixelRatioF(),
                                       palette().color(QPalette::Highlight),
                                       palette().color(QPalette::HighlightedText)));
    drag->setHotSpot(QPoint(12, 12));
    drag->exec(supportedActions, Qt::CopyAction);
}

void FileListView::dragEnterEvent(QDragEnterEvent *event) {
    if (fc::hasIncomingPaths(event->mimeData())) {
        event->acceptProposedAction();
        showDragFeedback(DragFeedbackState::Accepted, kDragFeedbackEnterDurationMs);
    } else {
        showDragFeedback(DragFeedbackState::Rejected, kDragFeedbackEnterDurationMs);
    }
}

void FileListView::dragMoveEvent(QDragMoveEvent *event) {
    if (fc::hasIncomingPaths(event->mimeData())) {
        event->acceptProposedAction();
        showDragFeedback(DragFeedbackState::Accepted, kDragFeedbackEnterDurationMs);
    } else {
        showDragFeedback(DragFeedbackState::Rejected, kDragFeedbackEnterDurationMs);
    }
}

void FileListView::dragLeaveEvent(QDragLeaveEvent *event) {
    // We draw no drop indicator, so skip QAbstractItemView's dragLeave handler,
    // which repaints the entire viewport. That full repaint fired every time a
    // fast drag crossed out of this view (e.g. over the splitter into the other
    // panel), causing a visible stutter. Accepting without a repaint is enough.
    clearDragFeedback();
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
    // Prefer the private format: a drag out of a network or archive panel puts
    // the backend's real paths there and nothing usable in the public URL list.
    const QStringList sourcePaths = fc::incomingPaths(event->mimeData());
    if (sourcePaths.isEmpty()) {
        clearDragFeedback();
        event->ignore();
        return;
    }

    const QString destDir = destinationDirForDrop(event->pos());
    if (destDir.isEmpty()) {
        clearDragFeedback();
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
        clearDragFeedback();
        event->ignore();
        return;
    }

    // The source panel's provider (null for an external drop): lets the receiver
    // route an archive/remote source through the right backend instead of reading
    // its virtual path as a local file.
    FileProvider *srcProvider = nullptr;
    if (auto *srcView = qobject_cast<QAbstractItemView *>(event->source()))
        if (auto *srcModel = qobject_cast<FileSystemModel *>(srcView->model()))
            srcProvider = srcModel->provider();
    emit filesDropped(sourcePaths, destDir, kind, srcProvider);
    event->acceptProposedAction();
    showDragFeedback(DragFeedbackState::Success, MotionPolicy::duration(MotionDuration::Normal));
}

void FileListView::paintEvent(QPaintEvent *event) {
    QTableView::paintEvent(event);
    if (m_dragFeedbackState == DragFeedbackState::None || m_dragFeedbackColor.alpha() == 0)
        return;

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(m_dragFeedbackColor, 2);
    if (m_dragFeedbackState == DragFeedbackState::Rejected)
        pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(viewport()->rect().adjusted(1, 1, -2, -2));
}

QColor FileListView::dragFeedbackColorFor(DragFeedbackState state) const {
    QColor color;
    switch (state) {
    case DragFeedbackState::Accepted:
        color = palette().color(QPalette::Highlight);
        color.setAlpha(210);
        break;
    case DragFeedbackState::Rejected:
        color = palette().color(QPalette::Text);
        color.setAlpha(180);
        break;
    case DragFeedbackState::Success:
        color = palette().color(QPalette::Highlight).lighter(125);
        color.setAlpha(220);
        break;
    case DragFeedbackState::None:
        break;
    }
    return color;
}

void FileListView::setDragFeedbackColor(const QColor &color) {
    if (m_dragFeedbackColor == color)
        return;
    m_dragFeedbackColor = color;
    viewport()->update();
}

void FileListView::showDragFeedback(DragFeedbackState state, int duration) {
    if (m_dragFeedbackState == state && duration > 0)
        return;

    m_dragFeedbackClearTimer->stop();
    const QColor target = dragFeedbackColorFor(state);
    const bool continueFromAccepted =
        state == DragFeedbackState::Success && m_dragFeedbackState == DragFeedbackState::Accepted;
    const QColor start = continueFromAccepted ? m_dragFeedbackColor : QColor(target.red(), target.green(), target.blue(), 0);
    m_dragFeedbackAnimation->stop();
    m_dragFeedbackState = state;

    if (MotionPolicy::reduced() || duration == 0) {
        setDragFeedbackColor(target);
        if (state == DragFeedbackState::Success) {
            // Keep the final color visible for the same brief success window.
            m_dragFeedbackClearTimer->setInterval(kDragFeedbackSuccessDurationMs);
            m_dragFeedbackClearTimer->start();
        }
        return;
    }

    setDragFeedbackColor(start);
    m_dragFeedbackAnimation->setDuration(duration);
    m_dragFeedbackAnimation->setEasingCurve(MotionPolicy::easing());
    m_dragFeedbackAnimation->setStartValue(start);
    m_dragFeedbackAnimation->setEndValue(target);
    m_dragFeedbackAnimation->start();
}

void FileListView::clearDragFeedback() {
    if (m_dragFeedbackState == DragFeedbackState::None && !m_dragFeedbackColor.isValid())
        return;
    m_dragFeedbackAnimation->stop();
    m_dragFeedbackClearTimer->stop();
    m_dragFeedbackState = DragFeedbackState::None;
    setDragFeedbackColor(QColor());
}

// PlainHeaderView declares Q_OBJECT (for the qproperty theme hooks) and lives in
// this .cpp, so AUTOMOC emits its meta-object here rather than in a header.
#include "FileListView.moc"
