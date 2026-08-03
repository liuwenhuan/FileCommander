#include "IconFileView.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QColor>
#include <QDrag>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "DragPixmap.h"
#include "ExternalPaths.h"
#include "FileSystemModel.h"
#include "MotionPolicy.h"

namespace {

// How long the view must stay still before its rows are treated as "what the
// user is looking at". Long enough not to fire mid-flick, short enough that
// letting go feels like it starts work immediately.
constexpr int kScrollSettleMs = 120;
constexpr int kDragFeedbackEnterDurationMs = 80;
constexpr int kDragFeedbackSuccessDurationMs = 150;

} // namespace

IconFileView::IconFileView(QWidget *parent)
    : QListView(parent), m_settleTimer(new QTimer(this)) {
    // No frame of its own. QTableView takes its border from the theme sheet, so
    // the list view drawing QFrame's default one as well put a second hairline
    // right above the status bar's own top border -- two lines where the detail
    // view shows one.
    setFrameShape(QFrame::NoFrame);
    // Drag source + drop target. The view mode, icon size and model are set up
    // by FilePanel, not here.
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(kScrollSettleMs);
    connect(m_settleTimer, &QTimer::timeout, this, &IconFileView::announceVisibleRange);

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
    connect(m_dragFeedbackClearTimer, &QTimer::timeout, this, &IconFileView::clearDragFeedback);

    MotionPolicy::observeReduced(this, [this](bool reduced) {
        if (reduced && m_dragFeedbackState != DragFeedbackState::None)
            showDragFeedback(m_dragFeedbackState, 0);
    });
}

QString IconFileView::dragFeedbackState() const {
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

void IconFileView::setModel(QAbstractItemModel *model) {
    clearDragFeedback();
    QListView::setModel(model);
    if (model)
        connect(model, &QAbstractItemModel::modelReset, this, &IconFileView::clearDragFeedback);
}

void IconFileView::keyPressEvent(QKeyEvent *event) {
    // Insert alongside Space -- see FileListView::keyPressEvent.
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Insert) {
        const QModelIndex index = currentIndex();
        if (index.isValid() && selectionModel()) {
            selectionModel()->select(index, QItemSelectionModel::Toggle);
            const QModelIndex next = index.sibling(index.row() + 1, index.column());
            if (next.isValid()) {
                selectionModel()->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
                scrollTo(next, QAbstractItemView::EnsureVisible);
            }
        }
        event->accept();
        return;
    }

    // Cursor-only navigation -- see FileListView::keyPressEvent for why. Left and
    // Right are included here because in a grid they are ordinary cursor
    // movement, unlike the detail list where they walk columns.
    if (event->modifiers() == Qt::NoModifier && selectionModel()) {
        CursorAction action = MoveDown;
        bool navigating = true;
        switch (event->key()) {
        case Qt::Key_Up: action = MoveUp; break;
        case Qt::Key_Down: action = MoveDown; break;
        case Qt::Key_Left: action = MoveLeft; break;
        case Qt::Key_Right: action = MoveRight; break;
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

    QListView::keyPressEvent(event);
}

void IconFileView::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    if (delta == 0)
        delta = event->pixelDelta().y();
    if ((event->modifiers() & Qt::ControlModifier) && delta != 0) {
        emit zoomRequested(delta > 0 ? 1 : -1);
        event->accept();
        return;
    }
    QListView::wheelEvent(event);
}

bool IconFileView::visibleRows(int *firstRow, int *lastRow) const {
    if (!model() || model()->rowCount() == 0)
        return false;

    // Probing the corners is not enough: in icon mode they usually land in the
    // spacing between items, where indexAt() reports nothing. Sample a grid of
    // points across the viewport and keep the extremes that hit an item.
    const QRect area = viewport()->rect();
    if (area.isEmpty())
        return false;

    int lo = -1;
    int hi = -1;
    // A sparse 8x8 probe can alias perfectly with a regular icon grid after a
    // scroll and land only in the gaps. Keep this bounded, but dense enough to
    // cross the smallest text/icon hit area used by the view. This only runs
    // after the settle timer, never for each scroll event.
    constexpr int kSamplesX = 32;
    constexpr int kSamplesY = 32;
    for (int iy = 0; iy <= kSamplesY; ++iy) {
        const int y = area.top() + iy * (area.height() - 1) / kSamplesY;
        for (int ix = 0; ix <= kSamplesX; ++ix) {
            const int x = area.left() + ix * (area.width() - 1) / kSamplesX;
            const QModelIndex idx = indexAt(QPoint(x, y));
            if (!idx.isValid())
                continue;
            if (lo < 0 || idx.row() < lo)
                lo = idx.row();
            if (idx.row() > hi)
                hi = idx.row();
        }
    }
    if (lo < 0)
        return false;

    *firstRow = lo;
    *lastRow = hi;
    return true;
}

void IconFileView::announceVisibleRange() {
    int first = 0, last = 0;
    if (visibleRows(&first, &last))
        emit visibleRangeSettled(first, last);
}

void IconFileView::scrollContentsBy(int dx, int dy) {
    QListView::scrollContentsBy(dx, dy);
    // Restart rather than fire: during a continuous scroll this keeps pushing
    // the deadline out, so the work starts once, where the user stopped.
    if (dx != 0 || dy != 0)
        m_settleTimer->start();
}

void IconFileView::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    // A resize relays out the grid, so a different set of rows is on screen.
    m_settleTimer->start();
}

void IconFileView::startDrag(Qt::DropActions supportedActions) {
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return;

    // IconMode selects items, not rows, so selectedRows() is empty here.
    // selectedIndexes() can also repeat a row across columns, so de-dup by row.
    QStringList paths;
    QSet<int> seenRows;
    QModelIndex firstIdx;
    for (const QModelIndex &idx : selectionModel()->selectedIndexes()) {
        if (seenRows.contains(idx.row()))
            continue;
        seenRows.insert(idx.row());
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
    // Same split as FileListView::startDrag: the private format holds the
    // backend's own paths for an in-app drop, while the public URL list only
    // ever names something that really resolves outside this process.
    fc::setPathPayload(mimeData, fsModel->provider(), paths, /*cut=*/false);

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    // Show what's actually being dragged: the first item's icon, plus a stacked
    // pile + count badge for a multi-item drag.
    const QIcon icon =
        fsModel->index(firstIdx.row(), FileSystemModel::NameColumn).data(Qt::DecorationRole).value<QIcon>();
    drag->setPixmap(ttc::makeDragPixmap(icon, paths.size(), devicePixelRatioF()));
    drag->setHotSpot(QPoint(12, 12));
    drag->exec(supportedActions, Qt::CopyAction);
}

void IconFileView::dragEnterEvent(QDragEnterEvent *event) {
    if (fc::hasIncomingPaths(event->mimeData())) {
        event->acceptProposedAction();
        showDragFeedback(DragFeedbackState::Accepted, kDragFeedbackEnterDurationMs);
    } else {
        showDragFeedback(DragFeedbackState::Rejected, kDragFeedbackEnterDurationMs);
    }
}

void IconFileView::dragMoveEvent(QDragMoveEvent *event) {
    if (fc::hasIncomingPaths(event->mimeData())) {
        event->acceptProposedAction();
        showDragFeedback(DragFeedbackState::Accepted, kDragFeedbackEnterDurationMs);
    } else {
        showDragFeedback(DragFeedbackState::Rejected, kDragFeedbackEnterDurationMs);
    }
}

void IconFileView::dragLeaveEvent(QDragLeaveEvent *event) {
    // We draw no drop indicator, so skip QAbstractItemView's dragLeave handler,
    // which repaints the entire viewport. That full repaint fired every time a
    // fast drag crossed out of this view (e.g. over the splitter into the other
    // panel), causing a visible stutter. Accepting without a repaint is enough.
    clearDragFeedback();
    event->accept();
}

QString IconFileView::destinationDirForDrop(const QPoint &pos) const {
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (!fsModel)
        return QString();

    const QModelIndex idx = indexAt(pos);
    if (idx.isValid() && fsModel->data(idx, FileSystemModel::IsDirRole).toBool())
        return fsModel->data(idx, FileSystemModel::FileInfoRole).toString();
    return fsModel->rootPath();
}

void IconFileView::dropEvent(QDropEvent *event) {
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

    FileListView::DropActionKind kind;
    if (sameView) {
        // In-panel drag: default move, Ctrl=copy, Shift=symlink.
        if (modifiers & Qt::ShiftModifier)
            kind = FileListView::DropActionKind::Link;
        else if (modifiers & Qt::ControlModifier)
            kind = FileListView::DropActionKind::Copy;
        else
            kind = FileListView::DropActionKind::Move;
    } else {
        // Cross-panel or from outside the app: default copy, Ctrl=move.
        Q_UNUSED(fromAnotherPanel);
        kind = (modifiers & Qt::ControlModifier) ? FileListView::DropActionKind::Move
                                                 : FileListView::DropActionKind::Copy;
    }

    // Dropping a selection onto itself (same dir) is a no-op, not an error.
    auto *fsModel = qobject_cast<FileSystemModel *>(model());
    if (sameView && fsModel && destDir == fsModel->rootPath()) {
        clearDragFeedback();
        event->ignore();
        return;
    }

    FileProvider *srcProvider = nullptr;
    if (auto *srcView = qobject_cast<QAbstractItemView *>(event->source()))
        if (auto *srcModel = qobject_cast<FileSystemModel *>(srcView->model()))
            srcProvider = srcModel->provider();
    emit filesDropped(sourcePaths, destDir, kind, srcProvider);
    event->acceptProposedAction();
    showDragFeedback(DragFeedbackState::Success, MotionPolicy::duration(MotionDuration::Normal));
}

void IconFileView::paintEvent(QPaintEvent *event) {
    QListView::paintEvent(event);
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

QColor IconFileView::dragFeedbackColorFor(DragFeedbackState state) const {
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

void IconFileView::setDragFeedbackColor(const QColor &color) {
    if (m_dragFeedbackColor == color)
        return;
    m_dragFeedbackColor = color;
    viewport()->update();
}

void IconFileView::showDragFeedback(DragFeedbackState state, int duration) {
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

void IconFileView::clearDragFeedback() {
    if (m_dragFeedbackState == DragFeedbackState::None && !m_dragFeedbackColor.isValid())
        return;
    m_dragFeedbackAnimation->stop();
    m_dragFeedbackClearTimer->stop();
    m_dragFeedbackState = DragFeedbackState::None;
    setDragFeedbackColor(QColor());
}
