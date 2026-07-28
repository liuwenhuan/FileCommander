#include <gtest/gtest.h>

#include <QByteArray>
#include <QImage>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>

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

TEST(PhosphorScanlines, VideoFilterUsesTintOnlyWithoutPerPixelScanlines) {
    const QColor tint(0x33, 0xff, 0x88);
    const QString filter = fc::mpvScanlinedPhosphorFilter(tint, 2, 0.25);

    // Keep the legacy preview entry point for QuickView, but video must use the
    // inexpensive tint-only path. Static QImages retain their scanlines above.
    EXPECT_EQ(filter, fc::mpvFilterFor(tint));
    EXPECT_TRUE(filter.startsWith(QStringLiteral("lavfi=[colorchannelmixer=")));
    EXPECT_EQ(filter.indexOf(QStringLiteral("geq=")), -1);
    EXPECT_EQ(filter.indexOf(QStringLiteral("mod(")), -1);
    EXPECT_EQ(filter.indexOf(QStringLiteral("Y\\,")), -1);
    EXPECT_EQ(filter.indexOf(QStringLiteral("scale")), -1);
    EXPECT_EQ(filter.indexOf(QStringLiteral("blend")), -1);
    EXPECT_TRUE(fc::mpvScanlinedPhosphorFilter(QColor()).isEmpty());
}

TEST(PhosphorScanlines, VideoFilterIsAcceptedByFfmpegWithoutScanlines) {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        GTEST_SKIP() << "ffmpeg is not installed";

    QString filter = fc::mpvScanlinedPhosphorFilter(QColor(0x33, 0xff, 0x88), 2, 0.25);
    ASSERT_TRUE(filter.startsWith(QStringLiteral("lavfi=[")));
    filter.chop(1);
    filter.remove(0, QStringLiteral("lavfi=[").size());

    QProcess process;
    process.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                           QStringLiteral("error"), QStringLiteral("-f"),
                           QStringLiteral("lavfi"), QStringLiteral("-i"),
                           QStringLiteral("color=c=red:s=4x4:d=0.04"),
                           QStringLiteral("-vf"), filter, QStringLiteral("-frames:v"),
                           QStringLiteral("1"), QStringLiteral("-pix_fmt"),
                           QStringLiteral("rgba"), QStringLiteral("-f"),
                           QStringLiteral("rawvideo"), QStringLiteral("-")});
    ASSERT_TRUE(process.waitForFinished(10'000));
    const QByteArray pixels = process.readAllStandardOutput();
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit) << process.readAllStandardError().toStdString();
    ASSERT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
    ASSERT_EQ(pixels.size(), 4 * 4 * 4);
    EXPECT_EQ(static_cast<unsigned char>(pixels.at(1)),
              static_cast<unsigned char>(pixels.at(4 * 4 + 1)));
}
