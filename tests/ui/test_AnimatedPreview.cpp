#include <gtest/gtest.h>

#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QVector>

#include "QuickView.h"
#include "ThemeStateGuard.h"
#include "config/Settings.h"

namespace {

// A real two-frame GIF, 8x8, the two frames a different flat colour each.
// Written by hand because this Qt cannot WRITE gif -- a fixture built with
// QImageWriter skipped every test instead of running one.
const unsigned char kAnimatedGif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x08, 0x00, 0x08, 0x00, 0xf0, 0x00, 0x00, 0xc0, 0x30,
    0x30, 0x30, 0xc0, 0x40, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45,
    0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00,
    0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x02, 0x0d, 0x8c, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x21, 0xf9, 0x04,
    0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00,
    0x02, 0x0d, 0x9c, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x29, 0x00,
    0x3b};

QString writeGif(const QTemporaryDir &dir, const QString &name) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(reinterpret_cast<const char *>(kAnimatedGif), sizeof(kAnimatedGif));
    return path;
}

QAction *actionNamed(QuickView &view, const QString &text) {
    for (QToolBar *bar : view.findChildren<QToolBar *>()) {
        for (QAction *action : bar->actions()) {
            if (action->text() == text)
                return action;
        }
    }
    return nullptr;
}

// By name, not by "has a pixmap". The transition snapshot is a QLabel with a
// pixmap too, so the loose search returned different widgets on different
// calls -- and an assertion that the frame had changed then passed by comparing
// two unrelated labels, animation or no animation.
QLabel *imageLabel(QuickView &view) {
    QLabel *label = view.findChild<QLabel *>(QStringLiteral("imagePreviewLabel"));
    return (label && label->pixmap() && !label->pixmap()->isNull()) ? label : nullptr;
}

} // namespace

// An animated GIF plays, and its controls are playback rather than rotation.
//
// Before this it went down the still path: one frame decoded, painted, done.
// The file looked like a picture of its first frame.
TEST(AnimatedPreview, AGifPlaysAndOffersPlaybackInsteadOfRotation) {
    ThemeStateGuard guard;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeGif(dir, QStringLiteral("a.gif"));
    ASSERT_FALSE(path.isEmpty());

    Settings settings(QDir(dir.path()).filePath(QStringLiteral("s.ini")));
    QWidget host;
    QuickView view(settings, QuickView::Context::Embedded, &host);
    host.resize(400, 300);
    host.show();
    view.resize(400, 300);
    view.show();
    view.showFile(path);

    QLabel *label = nullptr;
    QElapsedTimer budget;
    budget.start();
    while (!(label = imageLabel(view)) && budget.elapsed() < 4000)
        QTest::qWait(20);
    ASSERT_NE(label, nullptr) << "nothing was ever painted";

    // Asserted on pixels rather than on "play() was called": a call proves an
    // intention, a changed frame proves the animation.
    //
    // Counted over a window, not "changed at least once". The still path
    // repaints too -- the loader delivers an image and then a scaled one -- so
    // a single change proves nothing, and an earlier version of this passed
    // with the animation disabled. An animation keeps producing NEW pictures;
    // a still produces a bounded handful and then stops.
    //
    // Deliberately not "wait until the picture stops changing, then require
    // another change": that version hung. Waiting for an animation to go quiet
    // is waiting for something that by definition never happens, and the test
    // sat there for thirty-four minutes.
    QVector<QImage> seen;
    budget.restart();
    while (budget.elapsed() < 3000) {
        QTest::qWait(25);
        if (!label->pixmap())
            continue;
        const QImage now = label->pixmap()->toImage();
        if (seen.isEmpty() || now != seen.last())
            seen.append(now);
    }
    // The GIF is two frames on a loop, so three seconds of playing it yields
    // many alternations. A still settles at two or three and stays there.
    EXPECT_GE(seen.size(), 6)
        << "the picture changed " << seen.size()
        << " times in three seconds, which is a still settling rather than an animation";
    // Rotation is meaningless for a stream of frames and is out of the way;
    // playback is in its place.
    QAction *pause = actionNamed(view, QStringLiteral("Pause"));
    ASSERT_NE(pause, nullptr);
    EXPECT_TRUE(pause->isVisible());
    QAction *rotate = actionNamed(view, QStringLiteral("Rotate Left"));
    ASSERT_NE(rotate, nullptr);
    EXPECT_FALSE(rotate->isVisible()) << "a rotate control is offered for an animation";

    // Pausing stops the frames.
    pause->trigger();
    QTest::qWait(60);
    const QImage held = label->pixmap()->toImage();
    QTest::qWait(400);
    EXPECT_EQ(label->pixmap()->toImage(), held) << "paused, but the frames kept coming";
}

// A still keeps the controls it can use.
TEST(AnimatedPreview, AStillImageStillOffersRotation) {
    ThemeStateGuard guard;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString png = QDir(dir.path()).filePath(QStringLiteral("s.png"));
    QImage plain(24, 24, QImage::Format_RGB32);
    plain.fill(Qt::red);
    ASSERT_TRUE(plain.save(png));

    Settings settings(QDir(dir.path()).filePath(QStringLiteral("s.ini")));
    QWidget host;
    QuickView view(settings, QuickView::Context::Embedded, &host);
    host.resize(400, 300);
    host.show();
    view.resize(400, 300);
    view.show();
    view.showFile(png);

    QElapsedTimer budget;
    budget.start();
    while (!imageLabel(view) && budget.elapsed() < 4000)
        QTest::qWait(20);

    QAction *rotate = actionNamed(view, QStringLiteral("Rotate Left"));
    ASSERT_NE(rotate, nullptr);
    EXPECT_TRUE(rotate->isVisible()) << "a still lost its rotate control";
    QAction *pause = actionNamed(view, QStringLiteral("Pause"));
    ASSERT_NE(pause, nullptr);
    EXPECT_FALSE(pause->isVisible()) << "a still offers a pause control";
}

// An animation is a picture like any other: it zooms, and it reports what it is.
//
// The first version painted frames straight onto the label and bypassed the
// still-image path entirely. Zoom, fit and the info overlay all read the
// current image, so with that left empty a GIF could not be scaled and showed
// no dimensions -- reported from use, not caught here, because the first tests
// only asked whether the frames moved.
TEST(AnimatedPreview, AGifZoomsAndReportsItsSizeLikeAnyOtherPicture) {
    ThemeStateGuard guard;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeGif(dir, QStringLiteral("z.gif"));
    ASSERT_FALSE(path.isEmpty());

    Settings settings(QDir(dir.path()).filePath(QStringLiteral("s.ini")));
    QWidget host;
    QuickView view(settings, QuickView::Context::Embedded, &host);
    host.resize(400, 300);
    host.show();
    view.resize(400, 300);
    view.show();
    view.showFile(path);

    QLabel *label = nullptr;
    QElapsedTimer budget;
    budget.start();
    while (!(label = imageLabel(view)) && budget.elapsed() < 4000)
        QTest::qWait(20);
    ASSERT_NE(label, nullptr);

    // The overlay names the file and its dimensions. Empty text is what the
    // bypassed path produced.
    QLabel *info = view.findChild<QLabel *>(QStringLiteral("quickViewInfoOverlay"));
    ASSERT_NE(info, nullptr);
    budget.restart();
    while (info->text().isEmpty() && budget.elapsed() < 3000)
        QTest::qWait(20);
    EXPECT_FALSE(info->text().isEmpty()) << "the animation reported nothing about itself";
    EXPECT_TRUE(info->text().contains(QStringLiteral("z.gif")))
        << "overlay says: " << qPrintable(info->text());

    // Zooming in makes the frames bigger. Sampled over a window because the
    // next frame arrives continuously and replaces whatever was measured.
    // Through the toolbar, which is how a user zooms -- and it keeps the test
    // out of private methods.
    const int beforeWidth = label->pixmap()->width();
    QAction *zoomIn = actionNamed(view, QStringLiteral("Zoom In"));
    ASSERT_NE(zoomIn, nullptr);
    zoomIn->trigger();
    zoomIn->trigger();
    int widest = 0;
    budget.restart();
    while (budget.elapsed() < 2000) {
        QTest::qWait(25);
        if (label->pixmap())
            widest = qMax(widest, label->pixmap()->width());
    }
    EXPECT_GT(widest, beforeWidth)
        << "zoom did nothing: frames stayed " << beforeWidth << " px wide";
}
