#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QListView>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardItemModel>

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

} // namespace

// Thumbnails on a network share cost a round trip each, so a flick through a
// directory must not queue every row it sweeps past -- only where it lands.
TEST(IconFileViewTest, StaysQuietWhileScrollingAndReportsOnceAfterwards) {
    QStandardItemModel model(500, 1);
    IconFileView view;
    setUp(&view, &model);

    QSignalSpy spy(&view, &IconFileView::visibleRangeSettled);
    for (int i = 0; i < 12; ++i) {
        view.verticalScrollBar()->setValue(view.verticalScrollBar()->value() + 60);
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
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->value() + 500);
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
