#include <gtest/gtest.h>

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QSemaphore>
#include <QSet>
#include <QThread>

#include <atomic>
#include <cstring>
#include <memory>

#include "FileProvider.h"
#include "ThumbnailCache.h"

namespace {

struct MemHandle : public FileHandle {
    qint64 offset = 0;
};

class CountingImageProvider : public FileProvider {
public:
    explicit CountingImageProvider(QByteArray content) : m_content(std::move(content)) {}

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &path) const override { return path; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    bool canStream() const override { return true; }

    FileHandle *openRead(const QString &) override {
        ++m_openCount;
        return new MemHandle();
    }

    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override {
        ++m_readCount;
        auto *mem = static_cast<MemHandle *>(handle);
        const qint64 count = qMin<qint64>(m_content.size() - mem->offset, maxSize);
        if (count > 0) {
            std::memcpy(buffer, m_content.constData() + mem->offset,
                        static_cast<size_t>(count));
            mem->offset += count;
        }
        return count;
    }

    void closeHandle(FileHandle *handle) override { delete handle; }

    void resetCounts() {
        m_openCount = 0;
        m_readCount = 0;
    }
    int openCount() const { return m_openCount.load(); }
    int readCount() const { return m_readCount.load(); }

private:
    QByteArray m_content;
    std::atomic<int> m_openCount{0};
    std::atomic<int> m_readCount{0};
};

QByteArray pngBytes() {
    QImage image(240, 150, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, qRgb((x * 7) % 256, (y * 11) % 256, (x + y) % 256));
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

template <typename Predicate>
bool spinUntil(Predicate predicate, int budgetMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

QString uniqueConnection(const char *label) {
    static std::atomic<quint64> sequence{0};
    return QStringLiteral("smb://%1-%2-%3@fake")
        .arg(QLatin1String(label))
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++sequence);
}

QSet<QString> diskCacheFiles() {
    const QDir cache(ThumbnailCache::cacheDirectory());
    const QStringList names =
        cache.entryList({QStringLiteral("*.jpg"), QStringLiteral("*.png")}, QDir::Files);
    QSet<QString> paths;
    for (const QString &name : names)
        paths.insert(cache.absoluteFilePath(name));
    return paths;
}

QString seedDiskThumbnail(ThumbnailCache &cache,
                          const std::shared_ptr<CountingImageProvider> &provider,
                          const QString &connection, const QString &path) {
    const QSet<QString> before = diskCacheFiles();
    std::atomic<bool> ready{false};
    QObject context;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context,
                     [&](const QString &completed) { ready = ready || completed == path; });

    EXPECT_EQ(cache.requestRemoteThumbnail(provider, connection, path, 1700000000, 4096, 192),
              ThumbnailCache::Request::Queued);
    EXPECT_TRUE(spinUntil([&] { return ready.load(); }));

    const QSet<QString> added = diskCacheFiles() - before;
    EXPECT_EQ(added.size(), 1);
    return added.size() == 1 ? *added.constBegin() : QString();
}

class HookReset {
public:
    explicit HookReset(ThumbnailCache &cache) : m_cache(cache) {}
    ~HookReset() {
        m_release.release();
        m_cache.setDiskDecodeHookForTest({});
        m_cache.setPixmapInsertHookForTest({});
    }

    QSemaphore m_release;

private:
    ThumbnailCache &m_cache;
};

} // namespace

TEST(ThumbnailAsyncDiskHit, ValidRemoteHitDecodesOffGuiAndPromotesOnGuiWithoutProviderReads) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    auto provider = std::make_shared<CountingImageProvider>(pngBytes());
    const QString connection = uniqueConnection("valid");
    const QString path = QStringLiteral("/share/valid.png");
    ASSERT_FALSE(seedDiskThumbnail(cache, provider, connection, path).isEmpty());

    cache.setMemoryBudgetKiBForTest(ThumbnailCache::kMemoryBudgetKiB);
    cache.resetDiskDecodeCountForTest();
    provider->resetCounts();
    std::atomic<QThread *> decodeThread{nullptr};
    std::atomic<QThread *> insertionThread{nullptr};
    HookReset reset(cache);
    cache.setDiskDecodeHookForTest([&] { decodeThread = QThread::currentThread(); });
    cache.setPixmapInsertHookForTest([&] { insertionThread = QThread::currentThread(); });

    std::atomic<bool> notified{false};
    QObject context;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context,
                     [&](const QString &completed) { notified = notified || completed == path; });

    QPixmap ready;
    EXPECT_EQ(cache.requestRemoteThumbnail(provider, connection, path, 1700000000, 4096, 192,
                                            &ready),
              ThumbnailCache::Request::Queued);
    EXPECT_TRUE(ready.isNull());
    ASSERT_TRUE(spinUntil([&] { return notified.load(); }));

    EXPECT_NE(decodeThread.load(), qApp->thread());
    EXPECT_EQ(insertionThread.load(), qApp->thread());
    EXPECT_EQ(provider->openCount(), 0);
    EXPECT_EQ(provider->readCount(), 0);

    EXPECT_EQ(cache.requestRemoteThumbnail(provider, connection, path, 1700000000, 4096, 192,
                                            &ready),
              ThumbnailCache::Request::Ready);
    EXPECT_FALSE(ready.isNull());
}

TEST(ThumbnailAsyncDiskHit, CorruptPersistOnlyEntryIsRemovedAndRegeneratedExactlyOnce) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    auto provider = std::make_shared<CountingImageProvider>(pngBytes());
    const QString connection = uniqueConnection("corrupt");
    const QString path = QStringLiteral("/share/corrupt.png");
    const QString diskPath = seedDiskThumbnail(cache, provider, connection, path);
    ASSERT_FALSE(diskPath.isEmpty());

    QFile corrupt(diskPath);
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(corrupt.write("not an image"), 12);
    corrupt.close();
    cache.setMemoryBudgetKiBForTest(ThumbnailCache::kMemoryBudgetKiB);
    provider->resetCounts();

    std::atomic<bool> notified{false};
    QObject context;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context,
                     [&](const QString &completed) { notified = notified || completed == path; });

    EXPECT_EQ(cache.requestRemoteThumbnail(provider, connection, path, 1700000000, 4096, 192,
                                            nullptr,
                                            ThumbnailCache::CacheIntent::PersistOnly),
              ThumbnailCache::Request::Queued);
    ASSERT_TRUE(spinUntil([&] { return notified.load(); }));
    EXPECT_EQ(provider->openCount(), 1);
    EXPECT_GT(provider->readCount(), 0);
    EXPECT_FALSE(QImage(diskPath).isNull());
    EXPECT_EQ(cache.memoryStatsForTest().entries, 0);
    spinUntil([] { return false; }, 100);
    EXPECT_EQ(provider->openCount(), 1);
}

TEST(ThumbnailAsyncDiskHit, CancellationKeepsValidDiskEntryAndRejectsLatePromotion) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    auto provider = std::make_shared<CountingImageProvider>(pngBytes());
    const QString connection = uniqueConnection("cancel");
    const QString path = QStringLiteral("/share/cancel.png");
    const QString diskPath = seedDiskThumbnail(cache, provider, connection, path);
    ASSERT_FALSE(diskPath.isEmpty());

    cache.setMemoryBudgetKiBForTest(ThumbnailCache::kMemoryBudgetKiB);
    cache.resetDiskDecodeCountForTest();
    provider->resetCounts();
    std::atomic<int> insertions{0};
    std::atomic<bool> notified{false};
    QSemaphore decodeStarted;
    HookReset reset(cache);
    QObject context;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context,
                     [&](const QString &completed) { notified = notified || completed == path; });
    cache.setDiskDecodeHookForTest([&] {
        decodeStarted.release();
        reset.m_release.acquire();
    });
    cache.setPixmapInsertHookForTest([&] { ++insertions; });

    EXPECT_EQ(cache.requestRemoteThumbnail(provider, connection, path, 1700000000, 4096, 192),
              ThumbnailCache::Request::Queued);
    ASSERT_TRUE(decodeStarted.tryAcquire(1, 3000));
    cache.cancelRemote(provider.get());
    reset.m_release.release();
    ASSERT_TRUE(spinUntil([&] { return notified.load(); }, 3000));

    EXPECT_EQ(cache.diskDecodeCountForTest(), 1);
    EXPECT_EQ(insertions.load(), 0);
    EXPECT_EQ(provider->openCount(), 0);
    EXPECT_EQ(provider->readCount(), 0);
    EXPECT_TRUE(QFile::exists(diskPath));
    EXPECT_FALSE(QImage(diskPath).isNull());
}
