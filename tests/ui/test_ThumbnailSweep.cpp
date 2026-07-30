#include <gtest/gtest.h>

#include <QSet>
#include <QVector>

#include "ThumbnailSweep.h"

// The sweep decides what order a remote directory's thumbnails are fetched in.
// The contract it has to keep: the rows on screen come first, the whole listing
// eventually gets covered, no row is fetched twice, and scrolling re-aims it
// without losing or repeating work.
namespace {

// Drains the sweep, returning every row it hands out in order.
QVector<int> drain(ThumbnailSweep &sweep) {
    QVector<int> order;
    while (!sweep.complete()) {
        const int row = sweep.next();
        if (row < 0)
            break;
        order.append(row);
    }
    return order;
}

} // namespace

TEST(ThumbnailSweepTest, StartsAtTheVisibleRowsThenRunsToTheEnd) {
    ThumbnailSweep sweep;
    sweep.reset(10);
    sweep.focusOn(4, 6);

    const QVector<int> order = drain(sweep);
    // Visible row first, then downward -- what the user sees is served first.
    ASSERT_GE(order.size(), 6);
    EXPECT_EQ(order[0], 4);
    EXPECT_EQ(order[1], 5);
    EXPECT_EQ(order[5], 9); // reached the end of the listing
}

TEST(ThumbnailSweepTest, PrioritizesVisibleRowsThenTwoViewportRangesBeforeTheBackgroundSweep) {
    ThumbnailSweep sweep;
    sweep.reset(30);
    sweep.focusVisibleRowsWithAdjacentViewports(10, 12);

    const QVector<int> order = drain(sweep);
    const QVector<int> expectedForeground = {10, 11, 12, 13, 14, 15, 16, 17, 18,
                                             9, 8, 7, 6, 5, 4};
    ASSERT_GE(order.size(), expectedForeground.size());
    for (int i = 0; i < expectedForeground.size(); ++i)
        EXPECT_EQ(order[i], expectedForeground[i]);
}

TEST(ThumbnailSweepTest, MarksOnlyVisibleAndAdjacentRowsAsForegroundWork) {
    ThumbnailSweep sweep;
    sweep.reset(30);
    sweep.focusVisibleRowsWithAdjacentViewports(10, 12);

    for (int i = 0; i < 15; ++i) {
        bool foreground = false;
        EXPECT_NE(sweep.next(&foreground), -1);
        EXPECT_TRUE(foreground);
    }

    bool foreground = true;
    EXPECT_NE(sweep.next(&foreground), -1);
    EXPECT_FALSE(foreground);
}

TEST(ThumbnailSweepTest, WrapsAroundToFillTheRowsAboveTheStart) {
    ThumbnailSweep sweep;
    sweep.reset(10);
    sweep.focusOn(4, 6);

    const QVector<int> order = drain(sweep);
    // After the end it comes back for 0..3 -- the whole directory fills in,
    // which is the point of the sweep over the old visible-range-only prefetch.
    const QVector<int> expected = {4, 5, 6, 7, 8, 9, 0, 1, 2, 3};
    EXPECT_EQ(order, expected);
}

TEST(ThumbnailSweepTest, CoversEveryRowExactlyOnce) {
    ThumbnailSweep sweep;
    sweep.reset(500);
    sweep.focusOn(321, 340);

    const QVector<int> order = drain(sweep);
    EXPECT_EQ(order.size(), 500);
    EXPECT_EQ(QSet<int>(order.begin(), order.end()).size(), 500) << "a row was handed out twice";
    EXPECT_TRUE(sweep.complete());
    EXPECT_EQ(sweep.next(), -1) << "a completed sweep must not keep handing out rows";
}

TEST(ThumbnailSweepTest, ScrollingReaimsWithoutRepeatingFinishedRows) {
    ThumbnailSweep sweep;
    sweep.reset(100);

    // Fetch the first handful from the top of the listing.
    sweep.focusOn(0, 9);
    QSet<int> done;
    for (int i = 0; i < 10; ++i)
        done.insert(sweep.next());

    // User scrolls far down: the new visible rows must be served immediately,
    // not after grinding through the 80 rows in between.
    sweep.focusOn(90, 99);
    EXPECT_EQ(sweep.next(), 90) << "scrolling did not preempt the queued backlog";
    EXPECT_EQ(sweep.next(), 91);

    // And nothing already fetched is fetched again.
    const QVector<int> rest = drain(sweep);
    for (int row : rest)
        EXPECT_FALSE(done.contains(row)) << "row " << row << " was fetched twice";
}

TEST(ThumbnailSweepTest, RowsSkippedDuringAScrollAreStillPickedUpLater) {
    ThumbnailSweep sweep;
    sweep.reset(50);

    sweep.focusOn(0, 4);
    sweep.next(); // row 0
    sweep.next(); // row 1
    // Fast scroll past the middle of the listing.
    sweep.focusOn(40, 49);

    const QVector<int> rest = drain(sweep);
    // The rows flown past are not abandoned -- they come back around, which is
    // what makes the fill complete rather than leaving holes where the user
    // happened to scroll quickly.
    EXPECT_TRUE(rest.contains(20));
    EXPECT_TRUE(rest.contains(39));
    EXPECT_TRUE(sweep.complete());
}

TEST(ThumbnailSweepTest, PutBackMakesARowTheVeryNextOneAgain) {
    ThumbnailSweep sweep;
    sweep.reset(20);
    sweep.focusOn(5, 9);

    const int row = sweep.next();
    EXPECT_EQ(row, 5);
    // The caller could not place it (fetch queue full), so it hands it back;
    // it must be retried first, not lost and not deferred to the wrap-around.
    sweep.putBack(row);
    EXPECT_FALSE(sweep.complete());
    EXPECT_EQ(sweep.next(), 5);

    // Every row still gets covered exactly once overall.
    const QVector<int> rest = drain(sweep);
    QSet<int> all(rest.begin(), rest.end());
    all.insert(5);
    EXPECT_EQ(all.size(), 20);
}

// putBack() and focusOn() can arrive in either order once the pump is driven
// by events rather than a tight loop. Scrolling must win both ways: a row that
// merely failed to find a queue slot is less urgent than where the user is now
// looking. (Before this was fixed, the focusOn-then-putBack order rewound the
// cursor to the abandoned row and served row 0 next.)
TEST(ThumbnailSweepTest, ScrollingOutranksAPutBackRegardlessOfOrder) {
    {
        ThumbnailSweep sweep;
        sweep.reset(100);
        const int row = sweep.next();
        sweep.putBack(row);
        sweep.focusOn(90, 99);
        EXPECT_EQ(sweep.next(), 90) << "putBack-then-focusOn served the stale row";
    }
    {
        ThumbnailSweep sweep;
        sweep.reset(100);
        const int row = sweep.next();
        sweep.focusOn(90, 99);
        sweep.putBack(row);
        EXPECT_EQ(sweep.next(), 90) << "focusOn-then-putBack served the stale row";
    }
}

TEST(ThumbnailSweepTest, APutBackRowIsStillCoveredAfterBeingOutranked) {
    ThumbnailSweep sweep;
    sweep.reset(50);
    const int row = sweep.next(); // row 0
    sweep.focusOn(40, 49);
    sweep.putBack(row); // outranked, but must not be dropped

    const QVector<int> order = drain(sweep);
    EXPECT_TRUE(order.contains(row)) << "the put-back row was lost, not just deferred";
    EXPECT_EQ(QSet<int>(order.begin(), order.end()).size(), 50);
}

TEST(ThumbnailSweepTest, PutBackIgnoresRowsItNeverHandedOut) {
    ThumbnailSweep sweep;
    sweep.reset(10);

    const int before = sweep.remaining();
    sweep.putBack(3);   // never taken
    sweep.putBack(-1);  // out of range
    sweep.putBack(999); // out of range
    EXPECT_EQ(sweep.remaining(), before) << "putBack invented work that was never taken";
}

TEST(ThumbnailSweepTest, HandlesEmptyAndSingleRowListings) {
    ThumbnailSweep sweep;

    sweep.reset(0);
    EXPECT_TRUE(sweep.complete());
    EXPECT_EQ(sweep.next(), -1);
    sweep.focusOn(5, 10); // must not crash or index off the end
    EXPECT_EQ(sweep.next(), -1);

    sweep.reset(1);
    EXPECT_EQ(sweep.next(), 0);
    EXPECT_TRUE(sweep.complete());
}

TEST(ThumbnailSweepTest, ClampsAnOutOfRangeFocus) {
    ThumbnailSweep sweep;
    sweep.reset(10);

    // A stale visible range from a longer previous listing must not throw the
    // scan off the end of the current one.
    sweep.focusOn(99, 120);
    const int row = sweep.next();
    EXPECT_GE(row, 0);
    EXPECT_LT(row, 10);
    EXPECT_EQ(drain(sweep).size(), 9) << "the rest of the listing must still be covered";
}

TEST(ThumbnailSweepTest, ResetDiscardsPreviousProgress) {
    ThumbnailSweep sweep;
    sweep.reset(10);
    for (int i = 0; i < 5; ++i)
        sweep.next();
    ASSERT_EQ(sweep.remaining(), 5);

    // New directory: nothing carries over from the old one.
    sweep.reset(3);
    EXPECT_EQ(sweep.rowCount(), 3);
    EXPECT_EQ(sweep.remaining(), 3);
    EXPECT_EQ(drain(sweep), (QVector<int>{0, 1, 2}));
}
