#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRect>

#include "media/MediaTypes.h"
#include "media/WindowsMediaSurface.h"

TEST(WindowsMediaSurface, AppliesPhosphorTintFromSourceLuma) {
    QImage frame(2, 1, QImage::Format_ARGB32);
    frame.setPixel(0, 0, qRgba(255, 0, 0, 255));
    frame.setPixel(1, 0, qRgba(0, 0, 255, 255));

    VideoEffectSettings settings;
    settings.tint = QColor(0x33, 0xff, 0x88);
    settings.pixelBlock = 0;

    const QImage tinted = WindowsMediaSurface::applyVideoEffectForTest(frame, settings);

    ASSERT_EQ(tinted.size(), frame.size());
    const QColor redMapped = QColor::fromRgba(tinted.pixel(0, 0));
    const QColor blueMapped = QColor::fromRgba(tinted.pixel(1, 0));
    const int redLuma = qRound(255 * 0.299);
    const int blueLuma = qRound(255 * 0.114);
    EXPECT_GT(redMapped.green(), blueMapped.green());
    EXPECT_EQ(redMapped.red(), qRound(0x33 * (redLuma / 255.0)));
    EXPECT_EQ(redMapped.green(), qRound(0xff * (redLuma / 255.0)));
    EXPECT_EQ(redMapped.blue(), qRound(0x88 * (redLuma / 255.0)));
    EXPECT_EQ(blueMapped.red(), qRound(0x33 * (blueLuma / 255.0)));
    EXPECT_EQ(blueMapped.green(), qRound(0xff * (blueLuma / 255.0)));
    EXPECT_EQ(blueMapped.blue(), qRound(0x88 * (blueLuma / 255.0)));
}

TEST(WindowsMediaSurface, ChangingEffectRecomputesFromTheOriginalFrame) {
    QImage frame(1, 1, QImage::Format_ARGB32);
    frame.setPixel(0, 0, qRgba(255, 0, 0, 255));

    VideoEffectSettings green;
    green.tint = QColor(0, 255, 0);
    VideoEffectSettings amber;
    amber.tint = QColor(255, 160, 0);

    WindowsMediaSurface surface;
    surface.setVideoEffect(green);
    surface.setFrame(frame);
    surface.setVideoEffect(amber);

    const QColor mapped = QColor::fromRgba(surface.currentFrameForTest().pixel(0, 0));
    const int luma = qRound(255 * 0.299);
    EXPECT_EQ(mapped.red(), qRound(255 * (luma / 255.0)));
    EXPECT_EQ(mapped.green(), qRound(160 * (luma / 255.0)));
    EXPECT_EQ(mapped.blue(), 0);
}

TEST(WindowsMediaSurface, PaintsFrameAspectFitCenteredOnBlackBackground) {
    WindowsMediaSurface surface;
    surface.resize(100, 100);

    QImage frame(100, 50, QImage::Format_ARGB32);
    frame.fill(QColor(200, 20, 20));
    surface.setFrame(frame);

    QImage rendered(surface.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    surface.render(&painter);

    EXPECT_EQ(QColor::fromRgba(rendered.pixel(50, 10)), QColor(Qt::black));
    EXPECT_EQ(QColor::fromRgba(rendered.pixel(50, 25)), QColor(200, 20, 20));
    EXPECT_EQ(QColor::fromRgba(rendered.pixel(50, 74)), QColor(200, 20, 20));
    EXPECT_EQ(QColor::fromRgba(rendered.pixel(50, 90)), QColor(Qt::black));
}
