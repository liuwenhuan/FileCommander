#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QPixmap>

#include "FramelessChrome.h"

// The self-drawn window shadow was reported as far too heavy. The cause is not
// the peak ring alpha, which looks modest on its own -- it is that the rings
// are painted over one another, so whatever ramp they follow compounds at the
// content edge. A linear ramp piled up to a near-opaque black border there.
//
// These pin the compounded result rather than the constant, because the
// constant is not what anybody sees.
namespace {

// Alpha actually reached `distance` pixels outside the content rectangle, read
// straight off the rendered tile at the middle of its left edge -- away from
// the corners, where the rounding would confuse the reading.
int shadowAlphaOutsideContent(int distance) {
    const QImage tile =
        ttc::chrome::renderFrameTile(QColor(Qt::white), QPixmap()).toImage();
    const int x = ttc::chrome::kShadowMargin - distance;
    return qAlpha(tile.pixel(x, tile.height() / 2));
}

} // namespace

TEST(FramelessShadow, IsAShadowRatherThanABorderAtTheContentEdge) {
    // Just outside the window. 46% would read as a drawn outline; the old
    // linear ramp compounded to about 78% here.
    EXPECT_LT(shadowAlphaOutsideContent(1), 118);
    // Still present, though -- lightening it to nothing would have been a
    // different bug.
    EXPECT_GT(shadowAlphaOutsideContent(1), 25);
}

TEST(FramelessShadow, FadesToNothingByTheOuterEdgeOfItsMargin) {
    // The outermost ring sits against whatever is behind the window, so any
    // alpha there shows up as a hard line where the shadow stops.
    EXPECT_LE(shadowAlphaOutsideContent(ttc::chrome::kShadowMargin), 2);
    // And it has to get lighter the whole way out, or the gradient has a step
    // in it.
    int previous = 256;
    for (int d = 1; d <= ttc::chrome::kShadowMargin; ++d) {
        SCOPED_TRACE(d);
        const int alpha = shadowAlphaOutsideContent(d);
        EXPECT_LE(alpha, previous);
        previous = alpha;
    }
}
