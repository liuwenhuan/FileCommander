#include <gtest/gtest.h>

#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QSet>
#include <QImage>
#include <QPixmap>

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
