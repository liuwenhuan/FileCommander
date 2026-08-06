#include <gtest/gtest.h>

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QToolButton>
#include <QTemporaryDir>

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
