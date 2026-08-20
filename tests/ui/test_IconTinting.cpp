#include <gtest/gtest.h>

#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QSet>
#include <QImage>
#include <QPixmap>

#include <utility>

#include "filesystem/FileInfo.h"
#include "filesystem/IconCache.h"

// There are two kinds of icon and they must not share a tint.
//
// Our own SVG chrome (drive, computer, protocol glyphs) is drawn in one mid
// grey and looks dim on a dark background, so every theme recolours it. The
// system's file-type icons are full-colour artwork; recolouring THOSE is the
// CRT theme's whole point and nothing else's. One tint served both for a
// while, which is how the light theme came to draw every folder as a dark grey
// slab.
namespace {

FileInfo entry(const QString &name) {
    return FileInfo::fromFields(QStringLiteral("/") + name, name, 4096,
                                QDateTime::currentDateTime(), false, QFile::ReadOwner);
}

// The icon as pixels, so two lookups can be compared without depending on how
// colourful the artwork happens to be -- an unknown extension gets the generic
// page icon, which has almost no saturation to count hues in.
QImage pixels(const QIcon &icon) {
    return icon.pixmap(48, 48).toImage();
}

} // namespace

// Distinct suffixes throughout: the cache is keyed by extension, so reusing one
// would serve a later lookup from an earlier lookup's tint. All three are
// unknown to the shell, so untinted they are the same generic page icon --
// which is what makes them comparable.
TEST(IconTinting, AGlyphTintDoesNotReachTheSystemsFileIcons) {
    IconCache &cache = IconCache::instance();
    cache.setGlyphTint(QColor());
    cache.setFileIconTint(QColor());
    const QImage untouched = pixels(cache.iconFor(entry(QStringLiteral("sample.tinta"))));
    ASSERT_FALSE(untouched.isNull());

    cache.setGlyphTint(QColor(0x40, 0x40, 0x40)); // what the light theme sets
    cache.setFileIconTint(QColor());              // ...and no file-icon tint
    EXPECT_EQ(pixels(cache.iconFor(entry(QStringLiteral("sample.tintb")))), untouched)
        << "the light theme's glyph tint reached the file icons -- every folder "
           "came out a dark grey slab";

    cache.setGlyphTint(QColor());
    cache.setFileIconTint(QColor());
}

TEST(IconTinting, TheCrtThemeStillRecoloursFileIcons) {
    IconCache &cache = IconCache::instance();
    cache.setGlyphTint(QColor());
    cache.setFileIconTint(QColor());
    const QImage untouched = pixels(cache.iconFor(entry(QStringLiteral("sample.tintc"))));
    ASSERT_FALSE(untouched.isNull());

    cache.setGlyphTint(QColor(0x33, 0xff, 0x88));
    cache.setFileIconTint(QColor(0x33, 0xff, 0x88), 0);
    EXPECT_NE(pixels(cache.iconFor(entry(QStringLiteral("sample.tintd")))), untouched)
        << "a stylesheet cannot reach the system icon theme, so an untouched "
           "file icon is the one thing that gives the CRT theme away";

    // Leave the singleton as the other suites expect to find it.
    cache.setGlyphTint(QColor());
    cache.setFileIconTint(QColor());
}

// Our own chrome glyphs are drawn in one flat #888888. Run through the luma
// ramp that content uses, that grey lands at 63% of the tint -- a visibly
// faded icon beside label text painted in the same colour at full strength,
// which is what the function-key bar's two end buttons looked like.
TEST(IconTinting, AGlyphLandsOnTheThemeColourAtFullStrength) {
    IconCache &cache = IconCache::instance();
    const QColor phosphor(0x33, 0xff, 0x88);
    cache.setGlyphTint(phosphor);
    cache.setFileIconTint(QColor());

    const QImage image =
        cache.glyphIcon(QStringLiteral(":/icons/notepad.svg")).pixmap(48, 48).toImage();
    ASSERT_FALSE(image.isNull());

    // The most opaque pixel is the middle of a stroke: that one must BE the
    // theme colour, not a dimmed version of it. Edges stay soft because
    // anti-aliasing lives in alpha, which the recolour does not touch.
    int bestAlpha = 0;
    QColor solid;
    bool anySoftEdge = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > bestAlpha) {
                bestAlpha = pixel.alpha();
                solid = pixel;
            }
            if (pixel.alpha() > 8 && pixel.alpha() < 200)
                anySoftEdge = true;
        }
    }
    ASSERT_GT(bestAlpha, 200) << "the glyph did not render";
    EXPECT_EQ(solid.rgb(), phosphor.rgb())
        << "glyph ink is " << solid.name().toStdString() << ", theme colour is "
        << phosphor.name().toStdString();
    EXPECT_TRUE(anySoftEdge) << "anti-aliasing was flattened away with the colour";

    cache.setGlyphTint(QColor());
}

// A selected row in the CRT theme inverts: the fill becomes the very phosphor
// the glyph is drawn in, so the glyph disappears. Qt's own generated Selected
// pixmap blends 30% of the highlight over the icon, which on a monochrome glyph
// of that same colour changes nothing -- the variant has to be built here.
TEST(IconTinting, ASelectedGlyphIsDrawnInTheSelectionsTextColour) {
    IconCache &cache = IconCache::instance();
    const QColor phosphor(0x33, 0xff, 0x88);
    const QColor inverted(0x04, 0x14, 0x0a); // green.qss selection-color
    cache.setGlyphTint(phosphor, inverted);
    cache.setFileIconTint(QColor());

    // The middle of a stroke, as above: alpha is untouched by the recolour, so
    // the most opaque pixel is the one carrying the flat ink colour.
    const auto ink = [](const QImage &image) {
        int bestAlpha = 0;
        QColor solid;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > bestAlpha) {
                    bestAlpha = pixel.alpha();
                    solid = pixel;
                }
            }
        }
        return std::make_pair(bestAlpha, solid);
    };

    const QIcon icon = cache.glyphIcon(QStringLiteral(":/icons/dev-hdd.svg"));
    const auto normal = ink(icon.pixmap(48, 48, QIcon::Normal).toImage());
    const auto selected = ink(icon.pixmap(48, 48, QIcon::Selected).toImage());
    ASSERT_GT(normal.first, 200) << "the glyph did not render";
    ASSERT_GT(selected.first, 200) << "the selected glyph did not render";

    EXPECT_EQ(normal.second.rgb(), phosphor.rgb());
    EXPECT_EQ(selected.second.rgb(), inverted.rgb())
        << "selected glyph ink is " << selected.second.name().toStdString()
        << " -- on a " << phosphor.name().toStdString() << " fill that is invisible";
    // The thumbnail grid asks for no mode at all, and must keep the ordinary
    // colour: QIcon::pixmap() defaults to Normal.
    EXPECT_EQ(ink(icon.pixmap(48, 48).toImage()).second.rgb(), phosphor.rgb());

    // With no selected tint named there is no variant, and what Qt generates in
    // its place is the reason this exists: a 30% blend of the highlight over a
    // glyph already painted in that same colour, i.e. still a green on green.
    cache.setGlyphTint(phosphor);
    EXPECT_FALSE(cache.glyphSelectedTint().isValid());
    const QIcon plain = cache.glyphIcon(QStringLiteral(":/icons/dev-hdd.svg"));
    const QColor generated = ink(plain.pixmap(48, 48, QIcon::Selected).toImage()).second;
    EXPECT_GT(generated.lightness(), inverted.lightness() + 64)
        << "Qt's generated Selected pixmap is " << generated.name().toStdString()
        << " -- if it ever became readable on the selection fill, this whole "
           "variant could go";

    cache.setGlyphTint(QColor());
}
