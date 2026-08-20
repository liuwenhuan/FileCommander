#include <gtest/gtest.h>

#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "AnimatedImage.h"
#include "theme/Phosphor.h"

namespace {

// Real GIF bytes, written out per test.
//
// Built by hand rather than with QImageWriter: this Qt cannot WRITE gif at all
// (canWrite() is false), so a fixture that generated one skipped every test
// instead of running it. Two frames of a flat colour each, 8x8, looping.
const unsigned char kAnimatedGif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x08, 0x00, 0x08, 0x00, 0xf0, 0x00, 0x00, 0xc0,
    0x30, 0x30, 0x30, 0xc0, 0x40, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41,
    0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00,
    0x02, 0x19, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00,
    0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x08, 0x00, 0x00, 0x02, 0x19, 0x4c, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92,
    0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49,
    0x92, 0x24, 0x29, 0x00, 0x3b
};

const unsigned char kStillGif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x08, 0x00, 0x08, 0x00, 0xf0, 0x00, 0x00, 0xc0,
    0x30, 0x30, 0x30, 0xc0, 0x40, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41,
    0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00,
    0x02, 0x19, 0x4c, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24,
    0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x29, 0x00, 0x3b
};

QString writeGif(const QTemporaryDir &dir, const QString &name, const unsigned char *bytes,
                 int length) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(reinterpret_cast<const char *>(bytes), length);
    file.close();
    return path;
}

} // namespace

TEST(AnimatedImageTest, AMultiFrameGifIsAnimatedAndASingleFrameOneIsNot) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString many = writeGif(dir, QStringLiteral("many.gif"), kAnimatedGif, sizeof(kAnimatedGif));
    ASSERT_FALSE(many.isEmpty());
    EXPECT_TRUE(AnimatedImage::isAnimated(many));

    // A one-frame GIF is a still. supportsAnimation() says yes for every GIF --
    // it describes the format, not the file -- so answering from that alone
    // would send stills down a path that cannot rotate or zoom them.
    const QString one = writeGif(dir, QStringLiteral("one.gif"), kStillGif, sizeof(kStillGif));
    ASSERT_FALSE(one.isEmpty());
    EXPECT_FALSE(AnimatedImage::isAnimated(one));

    // And a PNG is not animated whatever its contents.
    const QString png = QDir(dir.path()).filePath(QStringLiteral("still.png"));
    QImage plain(16, 16, QImage::Format_RGB32);
    plain.fill(Qt::red);
    ASSERT_TRUE(plain.save(png));
    EXPECT_FALSE(AnimatedImage::isAnimated(png));
}

TEST(AnimatedImageTest, FramesArriveAndCarryTheThemeTint) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeGif(dir, QStringLiteral("a.gif"), kAnimatedGif, sizeof(kAnimatedGif));
    ASSERT_FALSE(path.isEmpty());

    const QColor previous = fc::previewTint();
    fc::setPreviewTint(QColor(0x33, 0xff, 0x88));

    AnimatedImage animation;
    QSignalSpy frames(&animation, &AnimatedImage::frameReady);
    ASSERT_TRUE(animation.play(path));

    // At least two distinct frames, or nothing is animating.
    QElapsedTimer budget;
    budget.start();
    while (frames.count() < 2 && budget.elapsed() < 4000)
        QTest::qWait(20);
    ASSERT_GE(frames.count(), 2) << "the animation delivered " << frames.count() << " frames";

    // Green-dominant: the source frames are not, so this is the tint and not
    // the picture.
    const QImage frame = frames.at(0).at(0).value<QImage>();
    ASSERT_FALSE(frame.isNull());
    const QColor pixel = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    EXPECT_GT(pixel.green(), pixel.red())
        << "frame came through untinted: " << qPrintable(pixel.name());

    fc::setPreviewTint(previous);
}

// Reports what the per-frame recolouring costs. Not a threshold: this exists so
// the number is on the record rather than guessed at, and so a change that makes
// it much worse is visible. Run with --gtest_also_run_disabled_tests.
TEST(AnimatedImageTest, DISABLED_TintCostPerFrame) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeGif(dir, QStringLiteral("cost.gif"), kAnimatedGif, sizeof(kAnimatedGif));
    ASSERT_FALSE(path.isEmpty());

    const QColor previous = fc::previewTint();
    fc::setPreviewTint(QColor(0x33, 0xff, 0x88));

    AnimatedImage animation;
    QSignalSpy frames(&animation, &AnimatedImage::frameReady);
    ASSERT_TRUE(animation.play(path));
    QElapsedTimer budget;
    budget.start();
    while (frames.count() < 20 && budget.elapsed() < 8000)
        QTest::qWait(10);
    std::fprintf(stderr, "8x8 frame: %lld us/frame over %d frames\n", animation.frameCostUs(),
                 frames.count());

    // The cost is per pixel, so scale the 8x8 figure to sizes worth knowing.
    // Measured directly rather than inferred: a full-size frame through the
    // same two passes.
    for (const QSize size : {QSize(320, 240), QSize(640, 480), QSize(1280, 720)}) {
        QImage frame(size, QImage::Format_ARGB32);
        frame.fill(QColor(120, 90, 60));
        QElapsedTimer timer;
        timer.start();
        constexpr int kRuns = 20;
        for (int i = 0; i < kRuns; ++i) {
            QImage copy = frame;
            fc::tintImage(copy, QColor(0x33, 0xff, 0x88));
            fc::applyScanlines(copy);
        }
        std::fprintf(stderr, "%4dx%-4d  %lld us/frame\n", size.width(), size.height(),
                     timer.nsecsElapsed() / 1000 / kRuns);
    }
    std::fflush(stderr);
    fc::setPreviewTint(previous);
}
