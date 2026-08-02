#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QListView>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTest>
#include <QWheelEvent>

#include "FilePanel.h"
#include "FileListView.h"
#include "FileSystemModel.h"
#include "IconFileView.h"

namespace {

// Spins the event loop for `ms`, so timers and layout actually run.
void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A grid view with enough rows to scroll through.
void setUp(IconFileView *view, QStandardItemModel *model) {
    for (int r = 0; r < 500; ++r)
        model->setItem(r, new QStandardItem(QStringLiteral("item %1").arg(r)));
    view->setModel(model);
    view->setViewMode(QListView::IconMode);
    view->setGridSize(QSize(100, 100));
    view->resize(520, 400);
    view->show();
    pump(250);
}

QScrollBar *activeScrollBar(IconFileView *view) {
    QScrollBar *vertical = view->verticalScrollBar();
    return vertical->maximum() > vertical->minimum() ? vertical : view->horizontalScrollBar();
}

bool sendWheel(QWidget *target, int angleDeltaY, Qt::KeyboardModifiers modifiers) {
    const QPointF local(target->rect().center());
    const QPointF global(target->mapToGlobal(local.toPoint()));
    QWheelEvent event(local, global, QPoint(), QPoint(0, angleDeltaY), Qt::NoButton,
                      modifiers, Qt::NoScrollPhase, false);
    event.setAccepted(false);
    QCoreApplication::sendEvent(target, &event);
    return event.isAccepted();
}

QSet<int> selectedRows(const QItemSelectionModel &selection) {
    QSet<int> rows;
    for (const QModelIndex &index : selection.selectedIndexes())
        rows.insert(index.row());
    return rows;
}

} // namespace

TEST(FileViewSelectionTest, SpaceTogglesCurrentListRowAndMovesDownWithoutChangingOtherSelections) {
    QStandardItemModel model(4, 3);
    FileListView view;
    view.setModel(&model);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);

    QItemSelectionModel *selection = view.selectionModel();
    const QModelIndex kept = model.index(0, 0);
    const QModelIndex current = model.index(2, 0);
    selection->select(kept, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);

    QTest::keyClick(&view, Qt::Key_Space);
    EXPECT_EQ(selectedRows(*selection), (QSet<int>{0, 2}));
    EXPECT_EQ(selection->currentIndex(), model.index(3, 0));

    selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    QTest::keyClick(&view, Qt::Key_Space);
    EXPECT_EQ(selectedRows(*selection), (QSet<int>{0}));
    EXPECT_EQ(selection->currentIndex(), model.index(3, 0));
}

TEST(FileViewSelectionTest, SpaceTogglesCurrentThumbnailAndMovesDownWithoutChangingOtherSelections) {
    QStandardItemModel model(4, 1);
    IconFileView view;
    view.setModel(&model);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);

    QItemSelectionModel *selection = view.selectionModel();
    const QModelIndex kept = model.index(0, 0);
    const QModelIndex current = model.index(2, 0);
    selection->select(kept, QItemSelectionModel::Select);
    selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);

    QTest::keyClick(&view, Qt::Key_Space);
    EXPECT_EQ(selectedRows(*selection), (QSet<int>{0, 2}));
    EXPECT_EQ(selection->currentIndex(), model.index(3, 0));

    selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    QTest::keyClick(&view, Qt::Key_Space);
    EXPECT_EQ(selectedRows(*selection), (QSet<int>{0}));
    EXPECT_EQ(selection->currentIndex(), model.index(3, 0));
}

TEST(FileViewWheelTest, CtrlWheelRequestsZoomAndOrdinaryWheelStillScrolls) {
    QStandardItemModel listModel(200, 1);
    FileListView list;
    list.setModel(&listModel);
    list.resize(320, 180);
    list.show();
    pump(50);
    QSignalSpy listZoom(&list, &FileListView::zoomRequested);

    EXPECT_TRUE(sendWheel(list.viewport(), 120, Qt::ControlModifier));
    EXPECT_TRUE(sendWheel(list.viewport(), -120, Qt::ControlModifier));
    ASSERT_EQ(listZoom.count(), 2);
    EXPECT_EQ(listZoom.at(0).at(0).toInt(), 1);
    EXPECT_EQ(listZoom.at(1).at(0).toInt(), -1);

    const int listBefore = list.verticalScrollBar()->value();
    sendWheel(list.viewport(), -120, Qt::NoModifier);
    EXPECT_GT(list.verticalScrollBar()->value(), listBefore);
    EXPECT_EQ(listZoom.count(), 2);

    QStandardItemModel iconModel;
    IconFileView icons;
    setUp(&icons, &iconModel);
    QSignalSpy iconZoom(&icons, &IconFileView::zoomRequested);

    EXPECT_TRUE(sendWheel(icons.viewport(), 120, Qt::ControlModifier));
    EXPECT_TRUE(sendWheel(icons.viewport(), -120, Qt::ControlModifier));
    ASSERT_EQ(iconZoom.count(), 2);
    EXPECT_EQ(iconZoom.at(0).at(0).toInt(), 1);
    EXPECT_EQ(iconZoom.at(1).at(0).toInt(), -1);

    QScrollBar *iconScrollBar = activeScrollBar(&icons);
    const int iconBefore = iconScrollBar->value();
    sendWheel(icons.viewport(), -120, Qt::NoModifier);
    EXPECT_GT(iconScrollBar->value(), iconBefore);
    EXPECT_EQ(iconZoom.count(), 2);
}

TEST(FileViewWheelTest, PanelRoutesCtrlWheelThroughExistingScaleControls) {
    FilePanel panel;
    panel.resize(800, 600);
    panel.show();
    pump(50);
    QSignalSpy scaleChanged(&panel, &FilePanel::viewScaleChanged);

    sendWheel(panel.view()->viewport(), 120, Qt::ControlModifier);
    const int enlargedRowHeight = panel.listRowHeight();
    EXPECT_GT(enlargedRowHeight, 0);
    sendWheel(panel.view()->viewport(), -120, Qt::ControlModifier);
    EXPECT_LT(panel.listRowHeight(), enlargedRowHeight);

    panel.toggleViewMode();
    ASSERT_NE(panel.iconView(), nullptr);
    const int initialThumbnailSize = panel.iconView()->iconSize().width();
    sendWheel(panel.iconView()->viewport(), 120, Qt::ControlModifier);
    EXPECT_GT(panel.thumbnailIconSize(), initialThumbnailSize);
    sendWheel(panel.iconView()->viewport(), -120, Qt::ControlModifier);
    EXPECT_EQ(panel.thumbnailIconSize(), initialThumbnailSize);
    EXPECT_EQ(scaleChanged.count(), 4);
}

// Thumbnails on a network share cost a round trip each, so a flick through a
// directory must not queue every row it sweeps past -- only where it lands.
TEST(IconFileViewTest, StaysQuietWhileScrollingAndReportsOnceAfterwards) {
    QStandardItemModel model(500, 1);
    IconFileView view;
    setUp(&view, &model);

    QSignalSpy spy(&view, &IconFileView::visibleRangeSettled);
    QScrollBar *scrollBar = activeScrollBar(&view);
    ASSERT_GT(scrollBar->maximum(), scrollBar->minimum());
    const int step = qMax(1, (scrollBar->maximum() - scrollBar->minimum()) / 24);
    for (int i = 0; i < 12; ++i) {
        scrollBar->setValue(scrollBar->value() + step);
        pump(20); // faster than the settle interval
    }
    EXPECT_EQ(spy.count(), 0) << "queued work mid-scroll for rows the user flew past";

    pump(400);
    EXPECT_EQ(spy.count(), 1) << "did not report the rows the scroll came to rest on";
}

// The reported window has to be the rows actually on screen, or the prefetch it
// drives would fetch the wrong ones.
TEST(IconFileViewTest, ReportsTheRowsThatAreOnScreen) {
    QStandardItemModel model(500, 1);
    IconFileView view;
    setUp(&view, &model);

    QSignalSpy spy(&view, &IconFileView::visibleRangeSettled);
    QScrollBar *scrollBar = activeScrollBar(&view);
    ASSERT_GT(scrollBar->maximum(), scrollBar->minimum());
    scrollBar->setValue(scrollBar->value() + 500);
    pump(400);
    ASSERT_GE(spy.count(), 1);

    const QList<QVariant> args = spy.takeLast();
    const int first = args.at(0).toInt();
    const int last = args.at(1).toInt();
    ASSERT_LE(first, last);

    // Checked by geometry: in icon mode a corner probe usually lands in the
    // spacing between items and finds nothing.
    const QRect firstRect = view.visualRect(model.index(first, 0));
    EXPECT_TRUE(firstRect.intersects(view.viewport()->rect()))
        << "reported a first row that is not on screen";
    if (first > 0) {
        const QRect before = view.visualRect(model.index(first - 1, 0));
        EXPECT_FALSE(before.intersects(view.viewport()->rect()))
            << "started later than it needed to, leaving a visible row unfetched";
    }
    EXPECT_LT(last - first, 200) << "window is far larger than a screenful";
}

// Resizing relays the grid out, so a different set of rows is showing.
TEST(IconFileViewTest, ReportsAfterAResize) {
    QStandardItemModel model(500, 1);
    IconFileView view;
    setUp(&view, &model);

    QSignalSpy spy(&view, &IconFileView::visibleRangeSettled);
    view.resize(300, 620);
    pump(400);
    EXPECT_GE(spy.count(), 1);
}

TEST(FilePanelStartupTest, IconViewIsCreatedOnceOnFirstSwitchWithCurrentState) {
    FilePanel panel;
    panel.setListFontFamily(QStringLiteral("Courier New"));
    panel.setListFontSize(17);
    QItemSelectionModel *selection = panel.view()->selectionModel();
    FileProvider *provider = panel.model()->provider();

    EXPECT_EQ(panel.iconView(), nullptr);
    panel.toggleViewMode();
    IconFileView *iconView = panel.iconView();
    ASSERT_NE(iconView, nullptr);
    EXPECT_TRUE(panel.isThumbnailMode());
    EXPECT_EQ(iconView->model(), panel.model());
    EXPECT_EQ(iconView->selectionModel(), selection);
    EXPECT_EQ(static_cast<FileSystemModel *>(iconView->model())->provider(), provider);
    EXPECT_EQ(iconView->font().family(), panel.view()->font().family());
    EXPECT_EQ(iconView->font().pointSize(), 17);
    EXPECT_EQ(iconView->viewport()->font().pointSize(), 17);

    panel.toggleViewMode();
    panel.toggleViewMode();
    EXPECT_EQ(panel.iconView(), iconView);
}
