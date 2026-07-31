// Covers the thumbnail cache's two size-related contracts:
//
//   * storageSize() rounds UP onto a rung ladder and never down -- the
//     invariant that keeps a stored bitmap at least as large as the one being
//     displayed, so it is only ever scaled down. Scaling up is what made
//     thumbnails blurry on a HiDPI display; no cache-sharing scheme may
//     reintroduce it.
//   * Zoom steps that land on the same rung share one stored thumbnail
//     instead of regenerating it. Every distinct icon size used to be its own
//     cache key, so each press of the zoom key re-fetched and re-decoded the
//     whole directory.

#include <gtest/gtest.h>

#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QPixmap>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "ThumbnailCache.h"

namespace {

// Redirects QStandardPaths (and so the thumbnail cache directory) at a
// throwaway location for the duration of one test, then restores it -- the
// cache is a process-wide singleton shared with the other thumbnail tests.
// (test_main.cpp now enables test mode for the whole run, so this is usually a
// no-op -- it restores whatever was in effect rather than forcing it off, which
// would hand every later test the developer's real cache directory back.)
class TestModePaths {
public:
    TestModePaths() : m_previous(QStandardPaths::isTestModeEnabled()) {
        QStandardPaths::setTestModeEnabled(true);
    }
    ~TestModePaths() { QStandardPaths::setTestModeEnabled(m_previous); }

private:
    bool m_previous;
};

class ScopedPath {
public:
    explicit ScopedPath(const QByteArray &value) : m_previous(qgetenv("PATH")) {
        qputenv("PATH", value);
    }
    ~ScopedPath() { qputenv("PATH", m_previous); }

private:
    QByteArray m_previous;
};

QString cacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/FileCommander/thumbnails");
}

int storedThumbnailCount() {
    return QDir(cacheDir())
        .entryList({QStringLiteral("*.jpg"), QStringLiteral("*.png")}, QDir::Files)
        .count();
}

// Lets any generation still in flight from an earlier test finish and write
// its file, so wiping afterwards leaves a genuinely empty directory. Without
// this the file counts below could pick up a late arrival from a neighbouring
// test and fail for a reason that has nothing to do with this one.
void drainPendingWork(int ms = 500) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

// Empties the thumbnail cache directory so the counts below start from zero.
void resetDiskCache() {
    drainPendingWork();
    QDir().mkpath(cacheDir());
    for (const QString &f : QDir(cacheDir())
                                .entryList({QStringLiteral("*.jpg"), QStringLiteral("*.png")},
                                           QDir::Files))
        QFile::remove(cacheDir() + QLatin1Char('/') + f);
}

// Spins the event loop until `path` has a thumbnail at `size`, or the deadline
// passes. Generation runs on a worker pool and reports back through a queued
// call, so the GUI thread has to be let run.
QPixmap waitForThumbnail(const QString &path, int size, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const QPixmap pm = ThumbnailCache::instance().thumbnail(path, size);
        if (!pm.isNull())
            return pm;
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return {};
}

// A real image file on disk: an ordinary local decode, so these tests never
// depend on ffmpeg being installed.
QString writeTestImage(const QDir &dir, const QString &name, int edge) {
    QImage image(edge, edge, QImage::Format_RGB32);
    for (int y = 0; y < edge; ++y) {
        for (int x = 0; x < edge; ++x)
            image.setPixel(x, y, qRgb((x * 7) % 256, (y * 11) % 256, ((x + y) * 3) % 256));
    }
    const QString path = dir.absoluteFilePath(name);
    return image.save(path, "PNG") ? path : QString();
}

// Every icon size FilePanel::zoomThumbnails() can produce: 48..192 by 16.
QVector<int> zoomLadder() {
    QVector<int> sizes;
    for (int px = 48; px <= 192; px += 16)
        sizes.append(px);
    return sizes;
}

} // namespace

// The never-upscale guarantee, stated directly. A stored thumbnail that came
// out SMALLER than the box it is painted into would have to be blown up, which
// is the defect this ladder exists to avoid.
TEST(ThumbnailStorageSizeTest, NeverReturnsLessThanRequested) {
    for (int requested = 1; requested <= 900; ++requested)
        ASSERT_GE(ThumbnailCache::storageSize(requested), requested) << "requested " << requested;
}

// Monotonic, so a larger display size never picks a smaller rung.
TEST(ThumbnailStorageSizeTest, IsMonotonic) {
    for (int requested = 2; requested <= 900; ++requested) {
        ASSERT_GE(ThumbnailCache::storageSize(requested),
                  ThumbnailCache::storageSize(requested - 1))
            << "at " << requested;
    }
}

// The point of the ladder: ten zoom steps must not mean ten stored bitmaps.
TEST(ThumbnailStorageSizeTest, CollapsesTheZoomLadderOntoFewRungs) {
    QSet<int> rungs;
    for (const int px : zoomLadder())
        rungs.insert(ThumbnailCache::storageSize(px));
    EXPECT_EQ(zoomLadder().size(), 10); // guards the assumption above
    EXPECT_LE(rungs.size(), 3) << "zoom steps still fragment the cache";

    // Same at a fractional HiDPI ratio, where the physical sizes are all
    // different numbers again.
    QSet<int> hidpiRungs;
    for (const int px : zoomLadder())
        hidpiRungs.insert(ThumbnailCache::storageSize(qRound(px * 1.5)));
    EXPECT_LE(hidpiRungs.size(), 3);
}

// The behaviour a user feels: after a directory is cached, changing zoom by one
// step is served from what is already stored rather than regenerating it.
TEST(ThumbnailCacheReuseTest, ZoomStepsOnOneRungShareASingleStoredThumbnail) {
    TestModePaths testPaths;
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    resetDiskCache();
    ASSERT_EQ(storedThumbnailCount(), 0);

    const QString image = writeTestImage(QDir(source.path()), QStringLiteral("a.png"), 400);
    ASSERT_FALSE(image.isEmpty());

    // Two icon sizes one zoom step apart that share a rung.
    const int small = 48;
    const int larger = 64;
    ASSERT_EQ(ThumbnailCache::storageSize(small), ThumbnailCache::storageSize(larger))
        << "test picked sizes that do not share a rung";

    const QPixmap first = waitForThumbnail(image, small);
    ASSERT_FALSE(first.isNull());
    EXPECT_EQ(storedThumbnailCount(), 1);

    // The second size is derived from the stored rung without regeneration,
    // but disk I/O and scaling complete asynchronously.
    EXPECT_TRUE(ThumbnailCache::instance().thumbnail(image, larger).isNull());
    const QPixmap second = waitForThumbnail(image, larger);
    EXPECT_FALSE(second.isNull()) << "a neighbouring zoom step had to regenerate";
    EXPECT_EQ(storedThumbnailCount(), 1) << "a second bitmap was stored for the same rung";

    // ...and it is delivered at the size asked for, not at the rung's size --
    // the delegate paints it 1:1, so anything else would be rescaled on screen.
    EXPECT_EQ(qMax(second.width(), second.height()), larger);
    EXPECT_EQ(qMax(first.width(), first.height()), small);
}

// Crossing to another rung is allowed to generate -- the sharing must not be
// achieved by quietly handing back a wrong-sized bitmap.
TEST(ThumbnailCacheReuseTest, CrossingARungStoresASecondThumbnail) {
    TestModePaths testPaths;
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    resetDiskCache();
    ASSERT_EQ(storedThumbnailCount(), 0);

    const QString image = writeTestImage(QDir(source.path()), QStringLiteral("b.png"), 800);
    ASSERT_FALSE(image.isEmpty());

    const int small = 48;
    const int big = 300;
    ASSERT_NE(ThumbnailCache::storageSize(small), ThumbnailCache::storageSize(big));

    ASSERT_FALSE(waitForThumbnail(image, small).isNull());
    EXPECT_EQ(storedThumbnailCount(), 1);

    const QPixmap large = waitForThumbnail(image, big);
    ASSERT_FALSE(large.isNull());
    EXPECT_EQ(storedThumbnailCount(), 2);
    EXPECT_EQ(qMax(large.width(), large.height()), big);
}

#ifdef Q_OS_WIN
TEST(ThumbnailCacheReuseTest, WindowsVideoThumbnailDoesNotNeedFfmpegOnPath) {
    TestModePaths testPaths;
    resetDiskCache();
    ScopedPath noFfmpeg{QByteArray()};

    const QString video =
        QDir::current().filePath(QStringLiteral("../../../wmf-fixtures-local-av/video-h264.mp4"));
    ASSERT_TRUE(QFileInfo::exists(video)) << video.toStdString();

    const QPixmap thumbnail = waitForThumbnail(video, 96);
    ASSERT_FALSE(thumbnail.isNull());
    EXPECT_LE(thumbnail.width(), 96);
    EXPECT_LE(thumbnail.height(), 96);
}
#endif
