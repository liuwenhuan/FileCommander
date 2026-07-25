#include <gtest/gtest.h>

#include <QPoint>
#include <QRect>

#include "SeekSlider.h"

// valueForClick is pure geometry math (the caller supplies the style's groove
// and handle rects), so it needs no QApplication.

// A groove 100..300 with a 20px handle: the handle centre can only reach
// 110..290, so those two points must map to the range ends exactly.
namespace {
const QRect kGroove(100, 0, 200, 20);
const QRect kHandle(100, 0, 20, 20);

int valueAt(int x, bool upsideDown = false) {
    return SeekSlider::valueForClick(kGroove, kHandle, QPoint(x, 10), Qt::Horizontal, 0, 1000,
                                     upsideDown);
}
} // namespace

TEST(SeekSliderTest, ClickMapsHandleTravelToFullRange) {
    EXPECT_EQ(valueAt(110), 0);    // leftmost reachable handle centre
    EXPECT_EQ(valueAt(290), 1000); // rightmost reachable handle centre
    EXPECT_EQ(valueAt(200), 500);  // midpoint of the travel
}

TEST(SeekSliderTest, ClickClampsOutsideHandleTravel) {
    // The groove edges lie half a handle beyond the travel, and clicks can also
    // arrive outside the widget entirely while dragging.
    EXPECT_EQ(valueAt(100), 0);
    EXPECT_EQ(valueAt(-50), 0);
    EXPECT_EQ(valueAt(300), 1000);
    EXPECT_EQ(valueAt(9999), 1000);
}

TEST(SeekSliderTest, UpsideDownMirrorsTheMapping) {
    // QStyleOptionSlider::upsideDown is what carries a right-to-left layout, so
    // the same click must read as the mirrored value.
    EXPECT_EQ(valueAt(110, true), 1000);
    EXPECT_EQ(valueAt(290, true), 0);
    EXPECT_EQ(valueAt(200, true), 500);
}

TEST(SeekSliderTest, VerticalUsesHeightNotWidth) {
    const QRect groove(0, 100, 20, 200);
    const QRect handle(0, 100, 20, 20);
    EXPECT_EQ(SeekSlider::valueForClick(groove, handle, QPoint(10, 110), Qt::Vertical, 0, 1000,
                                        false),
              0);
    EXPECT_EQ(SeekSlider::valueForClick(groove, handle, QPoint(10, 290), Qt::Vertical, 0, 1000,
                                        false),
              1000);
}

TEST(SeekSliderTest, RespectsANonZeroMinimum) {
    EXPECT_EQ(SeekSlider::valueForClick(kGroove, kHandle, QPoint(110, 10), Qt::Horizontal, 500,
                                        1500, false),
              500);
    EXPECT_EQ(SeekSlider::valueForClick(kGroove, kHandle, QPoint(200, 10), Qt::Horizontal, 500,
                                        1500, false),
              1000);
}
