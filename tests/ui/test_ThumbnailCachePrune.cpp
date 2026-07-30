// Covers the thumbnail disk cache's two eviction mechanisms, which exist
// because nothing used to remove a stored bitmap at all:
//
//   * the format stamp -- a cache written by a build that keyed or sized its
//     files differently is unreachable, and unreachable entries are never
//     looked up, so nothing would ever notice they are still on disk;
//   * the size cap -- reachable entries still accumulate, and the rung ladder
//     made each one an order of magnitude larger than the scheme it replaced.
//
// The third piece is the "last used" stamp the cap sorts on: a cache hit
// refreshes the file's mtime, but only when it is already a day stale, because
// the refresh sits on the scroll path.

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMap>
#include <QPixmap>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

#include "ThumbnailCache.h"

namespace {

QString cacheDir() { return ThumbnailCache::cacheDirectory(); }

QStringList thumbnailFilters() {
    return {QStringLiteral("*.jpg"), QStringLiteral("*.png")};
}

QStringList storedNames() {
    return QDir(cacheDir()).entryList(thumbnailFilters(), QDir::Files);
}

// Lets any generation still in flight from a neighbouring test land before this
// one counts files -- the cache is a process-wide singleton with a worker pool,
// so a late arrival would otherwise show up as this test's file.
void drainPendingWork(int ms = 500) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

// An empty cache directory with no format stamp: the state a fresh install is
// in, and the state every one of these tests starts from.
void resetCacheDir() {
    drainPendingWork();
    QDir dir(cacheDir());
    if (dir.exists())
        dir.removeRecursively();
    QDir().mkpath(cacheDir());
}

// Writes `bytes` bytes at `name` and back-dates it by `ageSecs`, so a test can
// state an LRU order directly instead of waiting for real time to pass.
QString writeCacheFile(const QString &name, int bytes, qint64 ageSecs) {
    const QString path = cacheDir() + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(QByteArray(bytes, 'x'));
    file.setFileTime(QDateTime::currentDateTime().addSecs(-ageSecs),
                     QFileDevice::FileModificationTime);
    file.close();
    return path;
}

QString cacheFileName(int index, const QString &extension = QStringLiteral("jpg")) {
    return QStringLiteral("2-%1.%2").arg(index, 32, 16, QLatin1Char('0')).arg(extension);
}

qint64 totalStoredBytes() {
    qint64 total = 0;
    QDir dir(cacheDir());
    for (const QString &name : storedNames())
        total += QFileInfo(dir.absoluteFilePath(name)).size();
    return total;
}

void writeStamp(const QByteArray &contents) {
    QFile file(cacheDir() + QStringLiteral("/version"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

// A real image on disk, so the thumbnail tests below are an ordinary local
// decode and never depend on ffmpeg.
QString writeTestImage(const QDir &dir, const QString &name, int edge) {
    QImage image(edge, edge, QImage::Format_RGB32);
    for (int y = 0; y < edge; ++y) {
        for (int x = 0; x < edge; ++x)
            image.setPixel(x, y, qRgb((x * 5) % 256, (y * 13) % 256, ((x * y) + 7) % 256));
    }
    const QString path = dir.absoluteFilePath(name);
    return image.save(path, "PNG") ? path : QString();
}

// Spins the event loop until `path` has a thumbnail: generation runs on a
// worker pool and reports back through a queued call.
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

// Back-dates a stored thumbnail so the next hit sees a stale "last used" stamp.
void backdate(const QString &path, qint64 secs) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.setFileTime(QDateTime::currentDateTime().addSecs(-secs),
                                 QFileDevice::FileModificationTime));
}

} // namespace

// The leak this layer exists to close: a cache left behind by a build that
// hashed its keys differently. Every file in it is unreachable, so no lookup
// will ever delete one.
TEST(ThumbnailCacheVersionTest, WipesACacheStampedByAnIncompatibleBuild) {
    resetCacheDir();
    for (int i = 0; i < 5; ++i)
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), 1024, 0).isEmpty());
    writeStamp("keys=999 rungs=7,7,7");

    EXPECT_EQ(ThumbnailCache::purgeIfStale(), 5);
    EXPECT_TRUE(storedNames().isEmpty()) << "an unreachable cache survived";
}

// An install from before the stamp existed looks exactly like a format change,
// because that is what it is -- nothing on disk says which format it is in.
TEST(ThumbnailCacheVersionTest, WipesAnUnstampedCache) {
    resetCacheDir();
    for (int i = 0; i < 4; ++i)
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), 512, 0).isEmpty());
    ASSERT_FALSE(QFileInfo::exists(cacheDir() + QStringLiteral("/version")));

    EXPECT_EQ(ThumbnailCache::purgeIfStale(), 4);
    EXPECT_TRUE(storedNames().isEmpty());
}

TEST(ThumbnailCacheVersionTest, LeavesFilesThatAreNotFileCommanderCacheEntries) {
    resetCacheDir();
    ASSERT_FALSE(writeCacheFile(cacheFileName(1, QStringLiteral("png")), 512, 0).isEmpty());
    const QString unrelated = writeCacheFile(QStringLiteral("other-app.png"), 512, 0);
    ASSERT_FALSE(unrelated.isEmpty());
    writeStamp("keys=999 rungs=7,7,7");

    EXPECT_EQ(ThumbnailCache::purgeIfStale(), 1);
    EXPECT_TRUE(QFileInfo::exists(unrelated)) << "purge deleted a file it does not own";
}

// ...and the far more common case: the stamp agrees, so nothing is touched.
// A purge that ran every launch would be worse than no purge at all.
TEST(ThumbnailCacheVersionTest, KeepsACacheThisBuildWrote) {
    resetCacheDir();
    ThumbnailCache::purgeIfStale(); // writes this build's stamp over the empty dir
    const QString stampPath = cacheDir() + QStringLiteral("/version");
    ASSERT_TRUE(QFileInfo::exists(stampPath)) << "purging did not leave a stamp behind";

    for (int i = 0; i < 3; ++i)
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), 1024, 0).isEmpty());

    EXPECT_EQ(ThumbnailCache::purgeIfStale(), 0);
    EXPECT_EQ(storedNames().size(), 3) << "a compatible cache was wiped";
}

// The stamp has to move when the RUNG LADDER moves, not only when the key
// format does. Changing a rung changes which file every request looks for just
// as thoroughly as rehashing the key would, and that half is easy to forget
// because it lives in storageSize() rather than anywhere near the stamp.
//
// Probed through storageSize() rather than against a hard-coded ladder, so
// editing the ladder does not by itself break this test -- only decoupling the
// stamp from it does.
TEST(ThumbnailCacheVersionTest, StampCoversTheRungLadder) {
    resetCacheDir();
    ThumbnailCache::purgeIfStale();

    QFile stamp(cacheDir() + QStringLiteral("/version"));
    ASSERT_TRUE(stamp.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(stamp.readAll());

    for (const int probe : {1, 97, 193, 385}) {
        const QString rung = QString::number(ThumbnailCache::storageSize(probe));
        EXPECT_TRUE(text.contains(rung))
            << "rung " << rung.toStdString() << " is not part of the format stamp: "
            << text.toStdString();
    }
}

// The cap, and the two properties that make it an LRU rather than an arbitrary
// cull: it deletes oldest-used first, and it stops below the limit rather than
// at it.
TEST(ThumbnailCachePruneTest, DeletesOldestFirstDownToTheHysteresisFloor) {
    resetCacheDir();
    constexpr int kFileBytes = 100 * 1024;
    constexpr int kFiles = 10;
    // Ages 10 days .. 1 day, so file0 is the coldest and file9 the warmest.
    for (int i = 0; i < kFiles; ++i) {
        const qint64 age = qint64(kFiles - i) * 24 * 60 * 60;
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), kFileBytes, age).isEmpty());
    }
    const qint64 limit = 500 * 1024; // floor is 400 KB, so 600 KB has to go
    ASSERT_EQ(totalStoredBytes(), qint64(kFiles) * kFileBytes);

    EXPECT_EQ(ThumbnailCache::pruneToLimit(limit), 6);
    EXPECT_LE(totalStoredBytes(), limit / 100 * 80) << "prune stopped above the floor";
    // Not far below it either: deleting past the floor throws away entries that
    // would have been served, which is the cost this cache exists to avoid.
    EXPECT_GT(totalStoredBytes(), limit / 100 * 80 - kFileBytes);

    const QStringList kept = storedNames();
    for (int i = 0; i < 6; ++i)
        EXPECT_FALSE(kept.contains(cacheFileName(i))) << "kept a colder entry";
    for (int i = 6; i < kFiles; ++i)
        EXPECT_TRUE(kept.contains(cacheFileName(i))) << "deleted a warmer entry";
}

TEST(ThumbnailCachePruneTest, LeavesACacheThatFitsAlone) {
    resetCacheDir();
    for (int i = 0; i < 5; ++i)
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), 10 * 1024, i).isEmpty());

    EXPECT_EQ(ThumbnailCache::pruneToLimit(1024 * 1024), 0);
    EXPECT_EQ(storedNames().size(), 5);
}

TEST(ThumbnailCachePruneTest, LeavesFilesThatAreNotFileCommanderCacheEntries) {
    resetCacheDir();
    ASSERT_FALSE(writeCacheFile(cacheFileName(1), 100 * 1024, 1).isEmpty());
    const QString unrelated = writeCacheFile(QStringLiteral("other-app.jpg"), 100 * 1024, 2);
    ASSERT_FALSE(unrelated.isEmpty());

    EXPECT_EQ(ThumbnailCache::pruneToLimit(50 * 1024), 1);
    EXPECT_TRUE(QFileInfo::exists(unrelated)) << "prune deleted a file it does not own";
}

// The stamp must outlive a prune. It is the oldest file in the directory by
// construction (written once, never touched again), so a sweep that treated it
// as an ordinary entry would delete it first -- and the next launch, finding no
// stamp, would wipe the whole cache it had just carefully trimmed.
TEST(ThumbnailCachePruneTest, NeverDeletesTheFormatStamp) {
    resetCacheDir();
    ThumbnailCache::purgeIfStale();
    const QString stampPath = cacheDir() + QStringLiteral("/version");
    backdate(stampPath, 365LL * 24 * 60 * 60);

    for (int i = 0; i < 4; ++i)
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), 100 * 1024, i).isEmpty());

    EXPECT_GT(ThumbnailCache::pruneToLimit(100 * 1024), 0); // forces a deep cull
    EXPECT_TRUE(QFileInfo::exists(stampPath)) << "the prune ate the format stamp";
}

// A hit has to record itself, or the prune's idea of "least recently used"
// degenerates into "written longest ago" and evicts exactly the entries a user
// keeps coming back to.
TEST(ThumbnailCacheTouchTest, ACacheHitRefreshesAStaleLastUsedStamp) {
    resetCacheDir();
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    const QString image = writeTestImage(QDir(source.path()), QStringLiteral("touch.png"), 300);
    ASSERT_FALSE(image.isEmpty());

    ASSERT_FALSE(waitForThumbnail(image, 48).isNull());
    ASSERT_EQ(storedNames().size(), 1);
    const QString stored = cacheDir() + QLatin1Char('/') + storedNames().first();

    backdate(stored, 3 * 24 * 60 * 60);
    ASSERT_GT(QFileInfo(stored).lastModified().secsTo(QDateTime::currentDateTime()),
              2 * 24 * 60 * 60);

    // Cleared so the lookup has to reach disk -- a memory hit never sees the
    // stored file at all, which is exactly why the throttle below is affordable.
    ThumbnailCache::instance().invalidateMemoryCache();
    ASSERT_FALSE(ThumbnailCache::instance().thumbnail(image, 48).isNull());

    EXPECT_LT(QFileInfo(stored).lastModified().secsTo(QDateTime::currentDateTime()), 60)
        << "a disk hit left the entry looking three days cold";
}

// ...and the other half: a warm entry is not rewritten. One screenful of an
// icon grid is dozens of disk hits, so an unthrottled touch would turn
// scrolling into a metadata write per file per frame.
TEST(ThumbnailCacheTouchTest, RepeatedHitsDoNotRewriteAWarmStamp) {
    resetCacheDir();
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    const QString image = writeTestImage(QDir(source.path()), QStringLiteral("warm.png"), 300);
    ASSERT_FALSE(image.isEmpty());

    ASSERT_FALSE(waitForThumbnail(image, 48).isNull());
    ASSERT_EQ(storedNames().size(), 1);
    const QString stored = cacheDir() + QLatin1Char('/') + storedNames().first();
    const QDateTime written = QFileInfo(stored).lastModified();

    for (int i = 0; i < 30; ++i) {
        ThumbnailCache::instance().invalidateMemoryCache(); // force a disk read
        ASSERT_FALSE(ThumbnailCache::instance().thumbnail(image, 48).isNull());
    }

    EXPECT_EQ(QFileInfo(stored).lastModified(), written)
        << "30 disk hits produced " << 30 << " metadata writes";
}

// The throttle at the scale it was written for: a screenful of an icon grid,
// scrolled repeatedly. Every pass here is a full round of disk hits (the memory
// cache is cleared between them, which is what scrolling far enough does for
// real), and the first two passes must cost nothing at all. Only the pass that
// finds the entries a day stale writes, and it writes once per file -- not once
// per pass, and not once per repaint.
TEST(ThumbnailCacheTouchTest, ScrollingCostsOneWritePerFilePerDayAtMost) {
    resetCacheDir();
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    constexpr int kFiles = 100;

    QStringList images;
    for (int i = 0; i < kFiles; ++i) {
        const QString path =
            writeTestImage(QDir(source.path()), QStringLiteral("grid%1.png").arg(i), 64);
        ASSERT_FALSE(path.isEmpty());
        images << path;
    }
    for (const QString &image : images)
        ASSERT_FALSE(waitForThumbnail(image, 48).isNull());
    ASSERT_EQ(storedNames().size(), kFiles);

    // One "scroll": drop the derived copies, then look every row up again.
    auto scrollPass = [&images] {
        ThumbnailCache::instance().invalidateMemoryCache();
        for (const QString &image : images)
            ThumbnailCache::instance().thumbnail(image, 48);
    };
    auto stamps = [] {
        QMap<QString, QDateTime> byName;
        QDir dir(cacheDir());
        for (const QString &name : storedNames())
            byName.insert(name, QFileInfo(dir.absoluteFilePath(name)).lastModified());
        return byName;
    };
    auto changedSince = [&stamps](const QMap<QString, QDateTime> &before) {
        int changed = 0;
        const QMap<QString, QDateTime> now = stamps();
        for (auto it = before.constBegin(); it != before.constEnd(); ++it)
            if (now.value(it.key()) != it.value())
                ++changed;
        return changed;
    };

    const QMap<QString, QDateTime> written = stamps();
    scrollPass();
    scrollPass();
    const int warmWrites = changedSince(written);

    // A day later, as far as the cache can tell.
    QDir dir(cacheDir());
    for (const QString &name : storedNames())
        backdate(dir.absoluteFilePath(name), 2 * 24 * 60 * 60);
    const QMap<QString, QDateTime> stale = stamps();
    scrollPass();
    const int staleWrites = changedSince(stale);
    const QMap<QString, QDateTime> refreshed = stamps();
    scrollPass();
    scrollPass();
    const int repeatWrites = changedSince(refreshed);

    std::cout << "[ touch    ] " << kFiles << " cached files: 2 warm scroll passes -> "
              << warmWrites << " writes, 1 stale pass -> " << staleWrites
              << " writes, 2 more passes -> " << repeatWrites << " writes" << std::endl;

    EXPECT_EQ(warmWrites, 0) << "scrolling a warm cache wrote to disk";
    EXPECT_EQ(staleWrites, kFiles) << "a stale entry was not refreshed on use";
    EXPECT_EQ(repeatWrites, 0) << "the refresh did not reset the throttle";
}

// How the default cap in Settings::thumbnailCacheLimitMb() was arrived at, and
// how to re-derive it. Disabled by default: it wants a corpus of real files
// (photographs compress nothing like a generated gradient) and it walks the
// whole zoom ladder over every one of them, which takes minutes rather than
// milliseconds.
//
//   FC_THUMB_CORPUS=/path/to/photos-and-videos \
//     ./ui_tests --gtest_also_run_disabled_tests \
//                --gtest_filter='*DISABLED_MeasuresRealCacheFootprint*'
//
// What it prints is the number the cap has to be set against: not the size of
// one stored thumbnail, but the total a *distinct file* accounts for once the
// zoom steps a user actually visits have each landed on their rung. Those are
// different by the number of rungs hit, and estimating the second from the
// first is how a sizing gets an order of magnitude wrong.
TEST(ThumbnailCachePruneTest, DISABLED_MeasuresRealCacheFootprint) {
    const QByteArray corpusPath = qgetenv("FC_THUMB_CORPUS");
    ASSERT_FALSE(corpusPath.isEmpty()) << "set FC_THUMB_CORPUS to a directory of real files";
    QDir corpus(QString::fromLocal8Bit(corpusPath));
    QStringList files;
    for (const QFileInfo &info : corpus.entryInfoList(QDir::Files, QDir::Name)) {
        if (ThumbnailCache::canThumbnail(info.absoluteFilePath()))
            files << info.absoluteFilePath();
    }
    ASSERT_FALSE(files.isEmpty());

    // Part one: what ONE stored bitmap costs at each rung. The mean is not
    // enough to size a cache by -- a directory of detailed photographs sits at
    // the top of this spread, and that is exactly the directory a cap has to
    // survive -- so the tail is printed too.
    for (const int rung : {96, 192, 384, 768}) {
        resetCacheDir();
        for (const QString &file : files)
            waitForThumbnail(file, rung, 60000);

        QVector<qint64> sizes;
        QDir dir(cacheDir());
        for (const QString &name : storedNames())
            sizes << QFileInfo(dir.absoluteFilePath(name)).size();
        ASSERT_FALSE(sizes.isEmpty());
        std::sort(sizes.begin(), sizes.end());
        qint64 total = 0;
        for (const qint64 size : sizes)
            total += size;
        std::cout << "[ rung     ] " << rung << " px: " << sizes.size() << " entries, mean "
                  << (total / sizes.size() / 1024) << " KB, median "
                  << (sizes[sizes.size() / 2] / 1024) << " KB, p90 "
                  << (sizes[sizes.size() * 9 / 10] / 1024) << " KB, max "
                  << (sizes.last() / 1024) << " KB" << std::endl;
    }

    // Part two: the zoom range FilePanel allows (48..192 logical, in 16px
    // steps) at a few device pixel ratios, because the ratio is what decides
    // which rungs those ten steps land on -- and so how many stored bitmaps a
    // file ends up with. This, not the figure above, is what a cap is set
    // against.
    for (const double dpr : {1.0, 1.5625, 2.0, 2.5}) {
        resetCacheDir();
        QMap<int, int> perRungCount;
        for (int logical = 48; logical <= 192; logical += 16) {
            const int physical = qRound(logical * dpr);
            perRungCount[ThumbnailCache::storageSize(physical)] = 0;
            for (const QString &file : files)
                waitForThumbnail(file, physical, 60000);
        }

        qint64 bytes = 0;
        QDir dir(cacheDir());
        for (const QString &name : storedNames())
            bytes += QFileInfo(dir.absoluteFilePath(name)).size();
        const int entries = storedNames().size();

        std::cout << "[ footprint] dpr " << dpr << ": " << files.size() << " files -> " << entries
                  << " entries (" << (double(entries) / files.size()) << " per file), "
                  << (bytes / 1024) << " KB total, " << (bytes / 1024 / files.size())
                  << " KB per file; rungs hit:";
        for (auto it = perRungCount.constBegin(); it != perRungCount.constEnd(); ++it)
            std::cout << ' ' << it.key();
        std::cout << std::endl;
    }
    resetCacheDir();
}

// Not an assertion about speed so much as a record of it: the prune stats every
// file in the directory, and whether that may sit on the startup path at all
// depends on what the number is at a realistic cache size. Printed rather than
// bounded -- a timing threshold in CI is a flake waiting to happen.
TEST(ThumbnailCachePruneTest, ScanAndDeleteCostAtRealisticSize) {
    resetCacheDir();
    constexpr int kFiles = 4000;
    constexpr int kFileBytes = 4 * 1024;
    for (int i = 0; i < kFiles; ++i)
        ASSERT_FALSE(writeCacheFile(cacheFileName(i), kFileBytes, i).isEmpty());

    const qint64 stored = qint64(kFiles) * kFileBytes;

    QElapsedTimer timer;
    timer.start();
    EXPECT_EQ(ThumbnailCache::pruneToLimit(stored * 2), 0); // scan only: nothing to delete
    const qint64 scanMs = timer.elapsed();

    timer.restart();
    const int removed = ThumbnailCache::pruneToLimit(stored / 2);
    const qint64 pruneMs = timer.elapsed();

    std::cout << "[ prune    ] " << kFiles << " files: scan " << scanMs << " ms, scan+delete of "
              << removed << " files " << pruneMs << " ms" << std::endl;
    EXPECT_GT(removed, 0);

    resetCacheDir(); // 4000 files is not something to leave behind
}
