#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QObject>
#include <QSet>

#include <atomic>
#include <cstring>
#include <memory>

#include "FileProvider.h"
#include "ThumbnailCache.h"
#include "ThumbnailSweep.h"

// The sweep's ordering is covered by test_ThumbnailSweep.cpp. This file covers
// the part that unit test cannot reach: the live drive chain that actually
// fills a directory -- requestRemoteThumbnail()'s four-state return, and the
// thumbnailReady -> pump loop that carries the fill past the first screenful.
// A stall anywhere in that chain leaves a directory permanently half-filled,
// and nothing in the pure-logic tests would notice.
namespace {

struct MemHandle : public FileHandle {
    qint64 offset = 0;
};

// Serves one in-memory JPEG for every path, counting opens so a test can tell
// a real fetch from a cache hit.
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

// A backend that cannot stream: every request for it must settle as Skipped,
// never Busy, or a sweep would retry it forever.
class NoStreamProvider : public ImageProvider {
public:
    NoStreamProvider() : ImageProvider(QByteArray("x")) {}
    bool canStream() const override { return false; }
};

QByteArray jpegBytes() {
    QImage image(120, 90, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            row[x] = qRgb((x * 7) % 256, (y * 5) % 256, (x + y) % 256);
    }
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 85);
    return out;
}

template <typename Pred>
bool spinUntil(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return pred();
}

// A distinct connection id per test keeps the on-disk cache from turning a
// later run into a false pass.
QString freshConnection() {
    return QStringLiteral("ftp://drive-%1@host").arg(QDateTime::currentMSecsSinceEpoch());
}

QString rowPath(int row) { return QStringLiteral("/share/row%1.jpg").arg(row); }

// Stands in for FilePanel::pumpThumbnailSweep: feeds rows to the cache in sweep
// order until it pushes back, putting the refused row back. Mirrors the panel's
// loop deliberately -- this is the logic under test.
int pumpOnce(ThumbnailSweep &sweep, ThumbnailCache &cache,
             const std::shared_ptr<FileProvider> &provider, const QString &connection) {
    int submitted = 0;
    while (!sweep.complete()) {
        const int row = sweep.next();
        if (row < 0)
            break;
        const auto outcome = cache.requestRemoteThumbnail(provider, connection, rowPath(row),
                                                          1700000000, 4096, 48);
        if (outcome == ThumbnailCache::Request::Busy) {
            sweep.putBack(row);
            break;
        }
        ++submitted;
    }
    return submitted;
}

} // namespace

TEST(ThumbnailSweepDriveTest, ReportsBusyOnceTheFetchQueueIsFull) {
    auto provider = std::make_shared<ImageProvider>(jpegBytes());
    ThumbnailCache &cache = ThumbnailCache::instance();
    const QString connection = freshConnection();

    // Far more rows than the fetcher's backlog: it must refuse at some point
    // rather than accept an unbounded queue. Busy is what tells the sweep to
    // stop and wait instead of spinning through the whole directory.
    bool sawBusy = false;
    for (int row = 0; row < 400 && !sawBusy; ++row) {
        sawBusy = cache.requestRemoteThumbnail(provider, connection, rowPath(row), 1700000000,
                                               4096, 48) == ThumbnailCache::Request::Busy;
    }
    EXPECT_TRUE(sawBusy) << "the fetch queue never pushed back; the backlog is unbounded";

    // The fetcher is a process-wide singleton, so leaving hundreds of requests
    // queued would starve whichever test runs next. Abandon them and let the
    // workers wind down before handing the fixture on.
    cache.cancelRemote(provider.get());
    spinUntil([&] { return false; }, 500);
}

TEST(ThumbnailSweepDriveTest, ABusyRowIsRetriedAndNotLost) {
    auto provider = std::make_shared<ImageProvider>(jpegBytes());
    ThumbnailCache &cache = ThumbnailCache::instance();
    const QString connection = freshConnection();

    ThumbnailSweep sweep;
    sweep.reset(60);

    // First pump fills the queue and stops early on a Busy.
    const int first = pumpOnce(sweep, cache, provider, connection);
    ASSERT_GT(first, 0);
    ASSERT_LT(first, 60) << "expected the queue to push back before the whole listing";
    const int remainingAfterFirst = sweep.remaining();

    // Now let the queue drain and pump again. Each round must place the rows
    // the previous one was refused -- this is the loop that fills a directory,
    // and a Busy that did not put its row back would stall it here. The spin
    // gives the fetch threads time to finish and deliver their results, which
    // is what frees the slots the next pump needs.
    for (int round = 0; round < 400 && !sweep.complete(); ++round) {
        spinUntil([&] { return false; }, 25);
        pumpOnce(sweep, cache, provider, connection);
    }

    EXPECT_TRUE(sweep.complete())
        << "the fill stalled with " << sweep.remaining() << " rows never submitted";
    EXPECT_LT(sweep.remaining(), remainingAfterFirst);
}

TEST(ThumbnailSweepDriveTest, ThumbnailReadyFiresAndCarriesTheFillForward) {
    auto provider = std::make_shared<ImageProvider>(jpegBytes());
    ThumbnailCache &cache = ThumbnailCache::instance();
    const QString connection = freshConnection();

    ThumbnailSweep sweep;
    sweep.reset(40);

    // This is the production wiring: every completion pumps the next row in.
    // If the signal never arrives, or the pump is not reached, the fill stops
    // dead after the first queueful -- the exact regression this guards.
    QObject context; // scopes the connection to this test (the cache is a singleton)
    std::atomic<int> completions{0};
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context, [&](const QString &) {
        ++completions;
        pumpOnce(sweep, cache, provider, connection);
    });

    pumpOnce(sweep, cache, provider, connection); // prime it; the signal does the rest
    EXPECT_TRUE(spinUntil([&] { return sweep.complete(); }, 30000))
        << "signal-driven fill stalled with " << sweep.remaining() << " rows left";
    EXPECT_GT(completions.load(), 0) << "thumbnailReady never fired";
}

TEST(ThumbnailSweepDriveTest, EveryRowEndsUpFetchedExactlyOnce) {
    auto provider = std::make_shared<ImageProvider>(jpegBytes());
    ThumbnailCache &cache = ThumbnailCache::instance();
    const QString connection = freshConnection();

    constexpr int kRows = 30;
    ThumbnailSweep sweep;
    sweep.reset(kRows);

    QObject context;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, &context, [&](const QString &) {
        pumpOnce(sweep, cache, provider, connection);
    });
    pumpOnce(sweep, cache, provider, connection);
    ASSERT_TRUE(spinUntil([&] { return sweep.complete(); }, 30000));

    // Let the last in-flight fetches land, then check the pixmaps really exist:
    // "submitted" is not the same as "arrived".
    spinUntil([&] { return false; }, 1500);
    int ready = 0;
    for (int row = 0; row < kRows; ++row) {
        if (!cache.remoteThumbnail(provider, connection, rowPath(row), 1700000000, 4096, 48)
                 .isNull())
            ++ready;
    }
    EXPECT_EQ(ready, kRows) << "only " << ready << "/" << kRows << " thumbnails materialised";
    // One fetch per row: the sweep must not re-request what it already has.
    EXPECT_EQ(provider->opens(), kRows);
}

TEST(ThumbnailSweepDriveTest, ANonStreamingBackendSettlesInsteadOfSpinning) {
    auto provider = std::make_shared<NoStreamProvider>();
    ThumbnailCache &cache = ThumbnailCache::instance();
    const QString connection = freshConnection();

    ThumbnailSweep sweep;
    sweep.reset(20);
    // Busy would mean "retry me", and a backend that can never stream would be
    // retried forever. The pump must run straight through to completion.
    const int submitted = pumpOnce(sweep, cache, provider, connection);
    EXPECT_EQ(submitted, 20);
    EXPECT_TRUE(sweep.complete()) << "a non-streaming backend stalled the sweep";
    EXPECT_EQ(provider->opens(), 0);
}

TEST(ThumbnailSweepDriveTest, CancellingTheProviderStopsTheFill) {
    auto provider = std::make_shared<ImageProvider>(jpegBytes());
    ThumbnailCache &cache = ThumbnailCache::instance();
    const QString connection = freshConnection();

    ThumbnailSweep sweep;
    sweep.reset(50);
    pumpOnce(sweep, cache, provider, connection);

    // Navigating away cancels the provider's work and resets the sweep; the
    // panel does both together. Nothing should keep feeding rows afterwards.
    cache.cancelRemote(provider.get());
    sweep.reset(0);

    EXPECT_TRUE(sweep.complete());
    EXPECT_EQ(pumpOnce(sweep, cache, provider, connection), 0)
        << "the sweep kept submitting after the listing was abandoned";
}
