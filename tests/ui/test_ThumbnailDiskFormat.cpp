#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>

#include <cmath>

#include "ThumbnailCache.h"

namespace {

class TestModePaths {
public:
    TestModePaths() : m_previous(QStandardPaths::isTestModeEnabled()) {
        QStandardPaths::setTestModeEnabled(true);
    }
    ~TestModePaths() { QStandardPaths::setTestModeEnabled(m_previous); }

private:
    bool m_previous;
};

void resetCacheDirectory() {
    QDir directory(ThumbnailCache::cacheDirectory());
    if (directory.exists())
        directory.removeRecursively();
    QDir().mkpath(ThumbnailCache::cacheDirectory());
}

QString cacheFilePath(const QString &key, const QString &extension) {
    const QString pattern = QStringLiteral("*-%1.%2").arg(key, extension);
    const QStringList matches =
        QDir(ThumbnailCache::cacheDirectory()).entryList({pattern}, QDir::Files);
    return matches.size() == 1
               ? QDir(ThumbnailCache::cacheDirectory()).absoluteFilePath(matches.constFirst())
               : QString();
}

QImage photographicFixture() {
    QImage image(320, 200, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double vignette = std::hypot(x - 160.0, y - 100.0) / 190.0;
            const int texture = int(10.0 * std::sin(x * 0.31) * std::cos(y * 0.17));
            const int red = qBound(0, int(210 - 90 * vignette + texture + y * 0.12), 255);
            const int green = qBound(0, int(155 - 70 * vignette + texture + x * 0.16), 255);
            const int blue = qBound(0, int(105 - 55 * vignette + texture + (x + y) * 0.09), 255);
            image.setPixel(x, y, qRgb(red, green, blue));
        }
    }
    return image;
}

double psnrRgb(const QImage &expected, const QImage &actual) {
    if (expected.size() != actual.size() || expected.isNull() || actual.isNull())
        return 0.0;

    double squaredError = 0.0;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const QRgb before = expected.pixel(x, y);
            const QRgb after = actual.pixel(x, y);
            const int dr = qRed(before) - qRed(after);
            const int dg = qGreen(before) - qGreen(after);
            const int db = qBlue(before) - qBlue(after);
            squaredError += dr * dr + dg * dg + db * db;
        }
    }
    const double meanSquaredError = squaredError / (expected.width() * expected.height() * 3.0);
    return meanSquaredError == 0.0 ? INFINITY : 10.0 * std::log10((255.0 * 255.0) / meanSquaredError);
}

} // namespace

TEST(ThumbnailDiskFormat, SelectsJpegForOpaqueImagesAndPngForTransparency) {
    QImage opaque(320, 200, QImage::Format_RGB32);
    opaque.fill(Qt::blue);
    EXPECT_EQ(ThumbnailCache::diskFormatFor(opaque), ThumbnailDiskFormat::Jpeg);

    QImage transparent(320, 200, QImage::Format_ARGB32);
    transparent.fill(QColor(0, 0, 255, 128));
    EXPECT_EQ(ThumbnailCache::diskFormatFor(transparent), ThumbnailDiskFormat::Png);
}

TEST(ThumbnailDiskFormat, PreservesAlphaInPngAndStoresOpaqueFixtureAsQualityJpeg) {
    TestModePaths testPaths;
    resetCacheDirectory();

    QImage transparent(64, 64, QImage::Format_ARGB32);
    transparent.fill(QColor(10, 20, 30, 128));
    ASSERT_TRUE(ThumbnailCache::saveThumbnail(transparent, QStringLiteral("alpha"),
                                              ThumbnailDiskFormat::Png));
    const QString alphaPath = cacheFilePath(QStringLiteral("alpha"), QStringLiteral("png"));
    ASSERT_FALSE(alphaPath.isEmpty());
    const QImage restoredAlpha(alphaPath);
    ASSERT_FALSE(restoredAlpha.isNull());
    EXPECT_EQ(restoredAlpha.pixelColor(0, 0).alpha(), 128);

    const QImage fixture = photographicFixture();
    ASSERT_TRUE(ThumbnailCache::saveThumbnail(fixture, QStringLiteral("photo-jpeg"),
                                              ThumbnailDiskFormat::Jpeg));
    ASSERT_TRUE(ThumbnailCache::saveThumbnail(fixture, QStringLiteral("photo-png"),
                                              ThumbnailDiskFormat::Png));
    const QString jpegPath = cacheFilePath(QStringLiteral("photo-jpeg"), QStringLiteral("jpg"));
    const QString pngPath = cacheFilePath(QStringLiteral("photo-png"), QStringLiteral("png"));
    ASSERT_FALSE(jpegPath.isEmpty());
    ASSERT_FALSE(pngPath.isEmpty());

    const QImage restoredJpeg(jpegPath);
    ASSERT_FALSE(restoredJpeg.isNull());
    EXPECT_GE(psnrRgb(fixture, restoredJpeg), 35.0);
    EXPECT_LT(QFileInfo(jpegPath).size(), QFileInfo(pngPath).size());
}

TEST(ThumbnailDiskFormat, SuccessfulWriteRemovesAlternateExtension) {
    TestModePaths testPaths;
    resetCacheDirectory();
    const QImage image = photographicFixture();

    ASSERT_TRUE(ThumbnailCache::saveThumbnail(image, QStringLiteral("alternate"),
                                              ThumbnailDiskFormat::Png));
    ASSERT_FALSE(cacheFilePath(QStringLiteral("alternate"), QStringLiteral("png")).isEmpty());
    ASSERT_TRUE(ThumbnailCache::saveThumbnail(image, QStringLiteral("alternate"),
                                              ThumbnailDiskFormat::Jpeg));
    EXPECT_TRUE(cacheFilePath(QStringLiteral("alternate"), QStringLiteral("png")).isEmpty());
    EXPECT_FALSE(cacheFilePath(QStringLiteral("alternate"), QStringLiteral("jpg")).isEmpty());
}
