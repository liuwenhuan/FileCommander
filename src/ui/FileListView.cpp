#include "FileListView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QColor>
#include <QDrag>
#include <QDropEvent>
#include <QFont>
#include <QHash>
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
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVector>

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
        // a genuine click: same section, negligible drag (not a resize/reorder).
        if (e->button() == Qt::LeftButton && m_pressIndex >= 0 &&
            logicalIndexAt(e->pos().x()) == m_pressIndex &&
            (e->pos() - m_pressPos).manhattanLength() < 4) {
            if (auto *view = qobject_cast<FileListView *>(parentWidget()))
                view->sortByHeaderSection(m_pressIndex);
        }
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
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    // Direct-painting delegate: keeps cells off QStyleSheetStyle's slow per-cell
    // CSS path so an interactive column/splitter resize stays smooth.
    setItemDelegate(new FileItemDelegate(this));
    // Always reserve the vertical scrollbar's space. Panels share one view whose
    // model swaps on tab switch; with an auto-hiding scrollbar the viewport width
    // would jump by ~15px between a tab that needs the bar and one that doesn't,
    // and stretchColumnsToFit() (which fits columns to viewport()->width()) would
    // reflow the columns on every switch. Keeping the bar always-on holds the
    // viewport width constant so column widths stay put.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalHeader()->hide();
    setHorizontalHeader(new PlainHeaderView(this)); // non-bold, self-painted labels
    horizontalHeader()->setSortIndicatorShown(true);
    setSortingEnabled(true); // header clicks call FileSystemModel::sort() automatically
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showColumnMenu(pos); });
    // A user drag of a column edge (not one of our programmatic resizes, which
    // set m_adjustingColumns) switches the panel to manual mode: from then on we
    // keep the user's widths and don't auto-fit to content on directory change.
    connect(horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int, int, int) {
                if (!m_adjustingColumns)
                    m_columnsManual = true;
            });

    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    // Debounces the full proportional column refit while a resize is in
    // flight; resizeEvent() does the cheap last-column-only variant per step.
    m_refitTimer = new QTimer(this);
    m_refitTimer->setSingleShot(true);
    m_refitTimer->setInterval(60);
    connect(m_refitTimer, &QTimer::timeout, this, [this] { stretchColumnsToFit(); });

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
        for (int col = 0; col < model->columnCount(); ++col)
            header->setSectionResizeMode(col, QHeaderView::Interactive);

        // Starting proportions; stretchColumnsToFit() scales these to fill the
        // panel. Name gets the lion's share.
        for (int col = 0; col < model->columnCount() && col < FileSystemModel::ColumnCount; ++col)
            header->resizeSection(col, kDefaultColWidths[col]);

        // Default view: hide Created and Permissions (a persisted header state,
        // restored later by MainWindow, overrides this if the user changed it).
        header->setSectionHidden(FileSystemModel::CreatedColumn, true);
        header->setSectionHidden(FileSystemModel::PermissionsColumn, true);

        stretchColumnsToFit();

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

        // On each directory load (the model resets), size the columns to their
        // content and scale to the viewport — unless the user has taken manual
        // control of the widths this session.
        connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            if (!m_columnsManual)
                fitColumnsToContents();
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
            stretchColumnsToFit();
        });
    }
    menu.exec(header->mapToGlobal(pos));
}

void FileListView::resizeEvent(QResizeEvent *event) {
    QTableView::resizeEvent(event);
    // During an interactive resize (window edge or splitter drag) this fires
    // per mouse step. A full proportional refit resizes every column — each
    // resizeSection triggering header/viewport relayout — which visibly lags
    // the drag. So per step only the last column absorbs the delta (one
    // section resize), and the full proportional refit runs once, shortly
    // after the size settles. The refit scales from the proportions captured
    // at the start of the burst (m_resizeBaseSizes), since the per-step
    // stretch skews the live last-column ratio.
    if (m_refitTimer) {
        if (!m_refitTimer->isActive()) {
            // First step of a burst: remember the current column proportions.
            m_resizeBaseSizes.clear();
            if (QHeaderView *header = horizontalHeader()) {
                m_resizeBaseSizes.reserve(header->count());
                for (int c = 0; c < header->count(); ++c)
                    m_resizeBaseSizes.append(header->sectionSize(c));
            }
        }
        m_refitTimer->start();
    }
    stretchLastColumnOnly();
}

void FileListView::stretchLastColumnOnly() {
    if (m_adjustingColumns)
        return;
    QHeaderView *header = horizontalHeader();
    if (!header || header->count() == 0)
        return;
    const int avail = viewport()->width();
    if (avail <= 0)
        return;

    int last = -1, othersTotal = 0;
    for (int c = 0; c < header->count(); ++c) {
        if (header->isSectionHidden(c))
            continue;
        if (last >= 0)
            othersTotal += header->sectionSize(last);
        last = c;
    }
    if (last < 0)
        return;

    m_adjustingColumns = true;
    header->resizeSection(last, qMax(30, avail - othersTotal));
    m_adjustingColumns = false;
}

void FileListView::fitColumnsToContents() {
    if (m_adjustingColumns)
        return;
    QHeaderView *header = horizontalHeader();
    if (!header || header->count() == 0 || !model())
        return;
    const int avail = viewport()->width();
    if (avail <= 0)
        return;

    // Desired width per visible column: content (+ header), but floored at the
    // column's baseline proportion so nothing collapses, and capped so one long
    // value can't swallow the row.
    QVector<int> cols;
    QVector<int> want;
    int total = 0;
    for (int c = 0; c < header->count(); ++c) {
        if (header->isSectionHidden(c))
            continue;
        const int content = sizeHintForColumn(c);    // samples visible rows
        const int head = header->sectionSizeHint(c); // header text width
        const int def = (c < FileSystemModel::ColumnCount) ? kDefaultColWidths[c] : 100;
        int w = qMax(def, qMax(content, head) + 14);
        w = qMin(w, def * 3); // cap growth so a long value doesn't dominate
        cols.append(c);
        want.append(w);
        total += w;
    }
    if (cols.isEmpty() || total <= 0)
        return;

    // Scale the desired widths to fill the viewport exactly (shrink if content
    // overflows -> no horizontal scrollbar; grow to use the extra space).
    m_adjustingColumns = true;
    const double factor = static_cast<double>(avail) / total;
    int used = 0;
    for (int i = 0; i < cols.size(); ++i) {
        const int w = (i == cols.size() - 1) ? qMax(30, avail - used)
                                             : qMax(30, static_cast<int>(want.at(i) * factor));
        used += w;
        header->resizeSection(cols.at(i), w);
    }
    m_adjustingColumns = false;
}

void FileListView::stretchColumnsToFit() {
    if (m_adjustingColumns)
        return;
    QHeaderView *header = horizontalHeader();
    if (!header || header->count() == 0)
        return;
    const int avail = viewport()->width();
    if (avail <= 0)
        return;

    // Prefer the proportions captured before the resize burst (see
    // resizeEvent); the live sizes have the last column skewed by the per-step
    // stretch. Outside a burst the vector is empty and live sizes are used.
    const bool useBase = m_resizeBaseSizes.size() == header->count();
    auto sizeOf = [&](int c) {
        return useBase ? m_resizeBaseSizes.at(c) : header->sectionSize(c);
    };

    QVector<int> cols;
    int total = 0;
    for (int c = 0; c < header->count(); ++c) {
        if (!header->isSectionHidden(c)) {
            cols.append(c);
            total += sizeOf(c);
        }
    }
    if (cols.isEmpty() || total <= 0)
        return;

    m_adjustingColumns = true;
    const double factor = static_cast<double>(avail) / total;
    int used = 0;
    for (int i = 0; i < cols.size(); ++i) {
        int width;
        if (i == cols.size() - 1)
            width = qMax(30, avail - used); // last column takes the remainder exactly
        else
            width = qMax(30, static_cast<int>(sizeOf(cols.at(i)) * factor));
        used += width;
        header->resizeSection(cols.at(i), width);
    }
    m_adjustingColumns = false;
    m_resizeBaseSizes.clear(); // burst finished; next one recaptures
}

void FileListView::setPanelActive(bool active) {
    if (property("panelActive").isValid() && property("panelActive").toBool() == active)
        return;
    setProperty("panelActive", active);
    // Re-run the QSS attribute selector so the selection colour updates now.
    // QStyleSheetStyle::polish() re-resolves the widget font from the global
    // stylesheet, discarding any point size set via setFont() -- which is what
    // reset the list to the default font the moment a file was selected (the
    // panel becoming active triggers this repolish). Preserve and restore the
    // user-chosen font across the repolish so the custom size sticks.
    const QFont keep = font();
    style()->unpolish(this);
    style()->polish(this);
    if (font() != keep)
        setFont(keep);
    viewport()->update();
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
