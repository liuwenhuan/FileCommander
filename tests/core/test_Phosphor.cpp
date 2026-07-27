#include <gtest/gtest.h>

#include <QImage>
#include <QPixmap>

#include "theme/Phosphor.h"

namespace {

QImage solidImage() {
    QImage image(2, 4, QImage::Format_ARGB32);
    image.fill(QColor(100, 120, 140, 120));
    return image;
}

} // namespace

TEST(PhosphorScanlines, AlternatesRowsWithoutChangingAlpha) {
    QImage image = solidImage();

    fc::applyScanlines(image, 2, 0.25);

    EXPECT_EQ(image.pixelColor(0, 0), QColor(75, 90, 105, 120));
    EXPECT_EQ(image.pixelColor(0, 1), QColor(100, 120, 140, 120));
    EXPECT_EQ(image.pixelColor(0, 2), QColor(75, 90, 105, 120));
    EXPECT_EQ(image.pixelColor(0, 3), QColor(100, 120, 140, 120));
}

TEST(PhosphorScanlines, DisabledScanlinesArePassThrough) {
    const QImage source = solidImage();
    QImage image = source;

    fc::applyScanlines(image, 1, 0.25);

    EXPECT_EQ(image, source);
}

TEST(PhosphorScanlines, PreviewPixmapPreservesDprAndTintPassThrough) {
    QPixmap source = QPixmap::fromImage(solidImage());
    source.setDevicePixelRatio(2.0);

    const QPixmap tinted =
        fc::scanlinedPhosphorPixmap(source, QColor(0x33, 0xff, 0x88), 2, 0.25);
    EXPECT_DOUBLE_EQ(tinted.devicePixelRatio(), 2.0);
    EXPECT_EQ(tinted.toImage().pixelColor(0, 0).alpha(), 120);
    EXPECT_LT(tinted.toImage().pixelColor(0, 0).green(),
              tinted.toImage().pixelColor(0, 1).green());
    EXPECT_EQ(fc::scanlinedPhosphorPixmap(source, QColor()).cacheKey(), source.cacheKey());
}

TEST(PhosphorScanlines, VideoFilterHasNoQuantisationStages) {
    const QString filter = fc::mpvScanlinedPhosphorFilter(QColor(0x33, 0xff, 0x88), 2, 0.25);

    EXPECT_TRUE(filter.startsWith(QStringLiteral("lavfi=[colorchannelmixer=")));
    EXPECT_NE(filter.indexOf(QStringLiteral("lutrgb=")), -1);
    EXPECT_NE(filter.indexOf(QStringLiteral("mod(y\\,2)")), -1);
    EXPECT_EQ(filter.indexOf(QStringLiteral("scale")), -1);
    EXPECT_EQ(filter.indexOf(QStringLiteral("blend")), -1);
    EXPECT_TRUE(fc::mpvScanlinedPhosphorFilter(QColor()).isEmpty());
}
