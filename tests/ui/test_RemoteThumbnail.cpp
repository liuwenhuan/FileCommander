#include <gtest/gtest.h>

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSignalSpy>

#include <atomic>
#include <cstring>
#include <memory>

#include "FileProvider.h"
#include "ThumbnailCache.h"

// End-to-end cover for the remote thumbnail path: bytes arrive over a
// FileProvider (no local file exists at the path at any point), and a real
// pixmap comes back out of the cache. The provider is a fake serving an
// in-memory PNG, but everything downstream of read() -- the fetch, the decode,
// the disk+memory cache, the thumbnailReady hop back to the GUI thread -- is
// the production code a real SMB/SFTP tab runs.
//
// Needs the QApplication that test_SeekSlider/QPixmap already require.
namespace {

struct MemHandle : public FileHandle {
    qint64 offset = 0;
};

class ImageProvider : public FileProvider {
public:
    explicit ImageProvider(QByteArray content) : m_content(std::move(content)) {}

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    bool canStream() const override { return true; }

    FileHandle *openRead(const QString &) override {
        ++m_opens;
        return new MemHandle();
    }
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override {
        auto *h = static_cast<MemHandle *>(handle);
        const qint64 n = qMin<qint64>(m_content.size() - h->offset, maxSize);
        if (n > 0) {
            std::memcpy(buffer, m_content.constData() + h->offset, static_cast<size_t>(n));
            h->offset += n;
        }
        return n;
    }
    void closeHandle(FileHandle *handle) override { delete handle; }

    int opens() const { return m_opens.load(); }

private:
    QByteArray m_content;
    std::atomic<int> m_opens{0};
};

// A recognisable 200x120 PNG, encoded to bytes so it only ever exists in
// memory -- if the cache ever fell back to the local path it would find
// nothing and the test would fail rather than silently pass.
QByteArray pngBytes() {
    QImage image(200, 120, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}

// Spins the event loop (the result comes back via a queued invocation) until
// `pred` holds or the budget runs out.
template <typename Pred>
bool spinUntil(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// A path that exists on no filesystem, so a local-path regression can't pass.
QString remotePath(const char *name) {
    return QStringLiteral("/ttc-test-share/%1").arg(QLatin1String(name));
}

} // namespace

TEST(RemoteThumbnailTest, ProducesAPixmapForAFileWithNoLocalPath) {
    auto provider = std::make_shared<ImageProvider>(pngBytes());
    const QString path = remotePath("photo.png");
    // A distinct connection id per run keeps the on-disk cache from turning a
    // later run into a false pass.
    const QString conn = QStringLiteral("smb://tester@fake-%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());

    ThumbnailCache &cache = ThumbnailCache::instance();
    // The delegate repaints off this signal, so a result nobody is told about
    // sits in the cache invisible until some unrelated repaint. Match on the
    // path: an earlier test's fetch may still be settling on the same signal.
    // `context` scopes the connection to this test -- the cache is a singleton
    // that outlives every test, so a context-less lambda capturing locals would
    // still be connected (and writing to dead stack) during later tests.
    QObject context;
    std::atomic<bool> notified{false};
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context,
                     [&](const QString &ready) { notified = notified || ready == path; });

    // First call is always a miss: it schedules the fetch and returns null.
    EXPECT_TRUE(cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64).isNull());
    ASSERT_TRUE(spinUntil([&] { return notified.load(); }, 10000))
        << "no thumbnailReady after the remote fetch";

    const QPixmap pixmap = cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64);
    ASSERT_FALSE(pixmap.isNull());
    // Scaled to fit the requested box, aspect ratio preserved (200x120 -> 64x38).
    EXPECT_LE(pixmap.width(), 64);
    EXPECT_LE(pixmap.height(), 64);
    EXPECT_GT(pixmap.width(), pixmap.height());
    EXPECT_EQ(provider->opens(), 1);
}

TEST(RemoteThumbnailTest, SecondRequestIsServedFromCacheWithoutRefetching) {
    auto provider = std::make_shared<ImageProvider>(pngBytes());
    const QString path = remotePath("cached.png");
    const QString conn = QStringLiteral("smb://tester@fake-%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());

    ThumbnailCache &cache = ThumbnailCache::instance();
    cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64);
    ASSERT_TRUE(spinUntil(
        [&] { return !cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64).isNull(); },
        10000));

    // Repainting the grid must not re-download: the whole point of the cache.
    for (int i = 0; i < 5; ++i)
        EXPECT_FALSE(cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64).isNull());
    EXPECT_EQ(provider->opens(), 1);
}

TEST(RemoteThumbnailTest, ChangedMtimeInvalidatesTheCachedEntry) {
    auto provider = std::make_shared<ImageProvider>(pngBytes());
    const QString path = remotePath("edited.png");
    const QString conn = QStringLiteral("smb://tester@fake-%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());

    ThumbnailCache &cache = ThumbnailCache::instance();
    cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64);
    ASSERT_TRUE(spinUntil(
        [&] { return !cache.remoteThumbnail(provider, conn, path, 1700000000, 4096, 64).isNull(); },
        10000));

    // A newer mtime is a different file as far as the key is concerned, so the
    // stale thumbnail must not be served for it.
    EXPECT_TRUE(cache.remoteThumbnail(provider, conn, path, 1700009999, 4096, 64).isNull());
    EXPECT_TRUE(spinUntil([&] { return provider->opens() == 2; }, 10000))
        << "an edited remote file kept showing its old thumbnail";
}

TEST(RemoteThumbnailTest, SamePathOnTwoServersGetsSeparateEntries) {
    auto red = std::make_shared<ImageProvider>(pngBytes());
    auto blue = std::make_shared<ImageProvider>(pngBytes());
    // Identical path and identical metadata: only the connection id differs, so
    // this is precisely the collision a path-only key would suffer.
    const QString path = remotePath("logo.png");
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    const QString connA = QStringLiteral("smb://a@host-%1").arg(stamp);
    const QString connB = QStringLiteral("sftp://b@host-%1").arg(stamp);

    ThumbnailCache &cache = ThumbnailCache::instance();
    // Waits on this file's own result rather than on any thumbnailReady, which
    // an unrelated fetch still settling from an earlier test could satisfy.
    cache.remoteThumbnail(red, connA, path, 1700000000, 4096, 64);
    ASSERT_TRUE(spinUntil(
        [&] { return !cache.remoteThumbnail(red, connA, path, 1700000000, 4096, 64).isNull(); },
        10000));

    // Server B's file must be fetched on its own account, not read off A's key.
    EXPECT_TRUE(cache.remoteThumbnail(blue, connB, path, 1700000000, 4096, 64).isNull());
    EXPECT_TRUE(spinUntil([&] { return blue->opens() == 1; }, 10000))
        << "the second server reused the first server's cached thumbnail";
}

TEST(RemoteThumbnailTest, OversizedImageIsSkippedWithoutTouchingTheNetwork) {
    auto provider = std::make_shared<ImageProvider>(pngBytes());
    const QString conn = QStringLiteral("smb://tester@fake-%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());

    // 64 MB claimed in the listing: far past the image budget, so the fetch must
    // never start -- an unattended grid scroll can't be allowed to pull that.
    const QPixmap pixmap = ThumbnailCache::instance().remoteThumbnail(
        provider, conn, remotePath("huge.png"), 1700000000, 64LL * 1024 * 1024, 64);
    EXPECT_TRUE(pixmap.isNull());

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(provider->opens(), 0) << "an oversized file was downloaded anyway";
}

TEST(RemoteThumbnailTest, NonImageAndDirectoryRequestsAreIgnored) {
    auto provider = std::make_shared<ImageProvider>(pngBytes());
    const QString conn = QStringLiteral("smb://tester@fake-%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());
    ThumbnailCache &cache = ThumbnailCache::instance();

    // Not a thumbnailable extension, and a zero size (what a directory entry
    // reports): both must short-circuit before any byte is requested.
    EXPECT_TRUE(cache.remoteThumbnail(provider, conn, remotePath("notes.txt"), 1, 4096, 64).isNull());
    EXPECT_TRUE(cache.remoteThumbnail(provider, conn, remotePath("photo.png"), 1, 0, 64).isNull());
    EXPECT_TRUE(cache.remoteThumbnail(nullptr, conn, remotePath("photo.png"), 1, 4096, 64).isNull());

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(provider->opens(), 0);
}
