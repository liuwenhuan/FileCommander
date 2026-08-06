#include <gtest/gtest.h>

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QToolButton>
#include <QTemporaryDir>

#include "AppIcon.h"
#include "MainWindow.h"
#include "ThemeStateGuard.h"
#include "config/Settings.h"

namespace {

// The colour of the most opaque pixel, which for a line glyph is the stroke.
QColor glyphColour(const QIcon &icon) {
    const QImage image =
        icon.pixmap(24, 24).toImage().convertToFormat(QImage::Format_ARGB32);
    int bestAlpha = -1;
    QRgb best = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) > bestAlpha) {
                bestAlpha = qAlpha(pixel);
                best = pixel;
            }
        }
    }
    return bestAlpha <= 0 ? QColor() : QColor(qRed(best), qGreen(best), qBlue(best));
}

} // namespace

// Chrome artwork that is recoloured from the palette must be recoloured by the
// time the window is first shown -- not only when the theme is changed later.
//
// The widgets are built long before the startup stylesheet is applied: a panel
// lands around 480 ms and the theme around 990 ms. The startup path applied the
// stylesheet directly instead of through applyTheme(), so nothing re-made the
// artwork, and these glyphs kept the flat #888888 they are drawn in for the
// whole session. Under Light and Dark that is a slightly-wrong grey; under
// Green CRT it is a grey icon surrounded by phosphor.
//
// Asserted on a FRESHLY CONSTRUCTED window with no theme change afterwards,
// because a theme change was always the thing that fixed it.
TEST(ThemedChromeArtwork, GlyphsAreThemedBeforeTheWindowIsEverShown) {
    ThemeStateGuard guard;

    // Written before the window exists: MainWindow reads the theme in its
    // constructor, and the constructor is what this test is about.
    {
        Settings settings;
        settings.setTheme(Settings::Theme::Crt);
    }

    MainWindow window;

    const QColor phosphor(0x33, 0xff, 0x88);
    const QList<QToolButton *> computerButtons =
        window.findChildren<QToolButton *>(QStringLiteral("PanelComputerButton"));
    ASSERT_FALSE(computerButtons.isEmpty());
    for (QToolButton *button : computerButtons) {
        const QColor shown = glyphColour(button->icon());
        ASSERT_TRUE(shown.isValid());
        EXPECT_EQ(shown, phosphor)
            << "the computer glyph is " << qPrintable(shown.name())
            << ", not the theme's " << qPrintable(phosphor.name());
    }
}

namespace {

// What fraction of the icon's box is actually painted.
qreal inkCoverage(const QIcon &icon, int size) {
    const QImage image =
        icon.pixmap(size, size).toImage().convertToFormat(QImage::Format_ARGB32);
    if (image.isNull())
        return 0.0;
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 128)
                ++painted;
        }
    }
    return qreal(painted) / qreal(image.width() * image.height());
}

} // namespace

// The app icon must not be a slab of near-black beside two-pixel line glyphs.
//
// Under Light the glyph colour is #404040 -- darker than the title bar it sits
// on. Flattening the filled mark to it produced the heaviest thing in the
// window. Dark and CRT tint towards the light, where the filled mark reads as
// lit rather than as a hole, so they are meant to keep it and this checks that
// too rather than only the case that was wrong.
// Asked through a real window under a real theme, NOT by calling appIcon()
// with the style spelled out. Passing the style in would test only that the
// painter can draw an outline when told to, and the thing that was wrong is
// which style the theme asks for -- a test that names the style itself stays
// green however that choice is wired.
TEST(ThemedChromeArtwork, TheAppIconIsDrawnInStrokesOnlyWhereItsColourIsDark) {
    ThemeStateGuard guard;

    auto inkForTheme = [](Settings::Theme theme) {
        {
            Settings settings;
            settings.setTheme(theme);
        }
        MainWindow window;
        return inkCoverage(window.windowIcon(), 24);
    };

    // Strokes on nothing: most of the box stays empty.
    const qreal lightInk = inkForTheme(Settings::Theme::Light);
    EXPECT_LT(lightInk, 0.45) << "under Light the mark covers " << lightInk
                              << " of its box, which is not an outline";

    // And the themes that tint towards the light keep the filled mark -- this
    // is not a change to all three.
    const qreal crtInk = inkForTheme(Settings::Theme::Crt);
    EXPECT_GT(crtInk, 0.55) << "under CRT the mark covers only " << crtInk;
}
