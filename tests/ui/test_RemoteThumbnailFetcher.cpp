#include <gtest/gtest.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

#include <atomic>
#include <cstring>
#include <memory>

#include "FileProvider.h"
#include "MpvStreamSource.h"
#include "RemoteThumbnailFetcher.h"

// There is no SMB/SFTP server available here, so these tests drive the fetcher
// through a fake provider that serves bytes from memory. That is exactly the
// surface the real backends expose (openRead/read/closeHandle), so the
// concurrency cap, the byte budget, and the cancellation path are all exercised
// for real -- only the wire is simulated.
namespace {

struct FakeHandle : public FileHandle {
    qint64 offset = 0;
};

// Serves `content` through the streaming API, counting how many reads it saw
// and optionally stalling each one so a test can observe overlap.
class FakeProvider : public FileProvider {
public:
    explicit FakeProvider(QByteArray content, int readDelayMs = 0)
        : m_content(std::move(content)), m_readDelayMs(readDelayMs) {}

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }

    bool canStream() const override { return true; }

    // Lets a test stand in for a backend with several independent read
    // channels (SMB's helper subprocesses) or just one (everything serial).
    int maxReadChannels() const override { return m_readChannels.load(); }
    void setReadChannels(int n) { m_readChannels.store(n); }

    FileHandle *openRead(const QString &) override {
        ++m_opens;
        return new FakeHandle();
    }

    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override {
        auto *h = static_cast<FakeHandle *>(handle);
        // Track the peak overlap so a test can assert the pool's cap holds.
        const int now = ++m_inRead;
        int prevPeak = m_peakConcurrent.load();
        while (now > prevPeak && !m_peakConcurrent.compare_exchange_weak(prevPeak, now)) {
        }
        if (m_readDelayMs > 0)
            QThread::msleep(static_cast<unsigned long>(m_readDelayMs));
        const qint64 remaining = m_content.size() - h->offset;
        const qint64 n = qMin(remaining, maxSize);
        if (n > 0) {
            std::memcpy(buffer, m_content.constData() + h->offset, static_cast<size_t>(n));
            h->offset += n;
        }
        m_bytesServed += n;
        --m_inRead;
        return n;
    }

    void closeHandle(FileHandle *handle) override { delete handle; }

    int opens() const { return m_opens.load(); }
    qint64 bytesServed() const { return m_bytesServed.load(); }
    int peakConcurrent() const { return m_peakConcurrent.load(); }

private:
    QByteArray m_content;
    int m_readDelayMs;
    std::atomic<int> m_opens{0};
    std::atomic<qint64> m_bytesServed{0};
    std::atomic<int> m_inRead{0};
    std::atomic<int> m_peakConcurrent{0};
    std::atomic<int> m_readChannels{1};
};

// A provider that cannot stream -- submit() must refuse it outright rather than
// queue a job that could never do anything.
class NoStreamProvider : public FakeProvider {
public:
    NoStreamProvider() : FakeProvider(QByteArray("data")) {}
    bool canStream() const override { return false; }
};

QByteArray payload(int size) {
    QByteArray data;
    data.resize(size);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>(i % 251);
    return data;
}

// Busy-waits (without an event loop -- the fetcher signals nothing) until
// `pred` holds or the budget runs out.
template <typename Pred>
bool waitFor(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs)
        QThread::msleep(5);
    return pred();
}

} // namespace

TEST(RemoteThumbnailFetcherTest, DownloadStopsAtTheByteBudget) {
    auto provider = std::make_shared<FakeProvider>(payload(1024 * 1024));
    RemoteThumbnailFetcher fetcher;

    std::atomic<bool> done{false};
    QString fetched;
    ASSERT_TRUE(fetcher.submit(provider, [&](const RemoteThumbnailFetcher::Ticket &ticket) {
        fetched = ticket.download(QStringLiteral("/share/big.jpg"), 128 * 1024);
        done = true;
    }));
    ASSERT_TRUE(waitFor([&] { return done.load(); }, 5000));

    ASSERT_FALSE(fetched.isEmpty());
    // A budget is a hard stop, not a hint: the rest of the file is never pulled.
    EXPECT_EQ(QFile(fetched).size(), 128 * 1024);
    EXPECT_EQ(provider->bytesServed(), 128 * 1024);
    QFile::remove(fetched);
}

TEST(RemoteThumbnailFetcherTest, DownloadStopsAtEofWhenSmallerThanBudget) {
    auto provider = std::make_shared<FakeProvider>(payload(4096));
    RemoteThumbnailFetcher fetcher;

    std::atomic<bool> done{false};
    QString fetched;
    ASSERT_TRUE(fetcher.submit(provider, [&](const RemoteThumbnailFetcher::Ticket &ticket) {
        fetched = ticket.download(QStringLiteral("/share/small.jpg"), 8 * 1024 * 1024);
        done = true;
    }));
    ASSERT_TRUE(waitFor([&] { return done.load(); }, 5000));

    ASSERT_FALSE(fetched.isEmpty());
    EXPECT_EQ(QFile(fetched).size(), 4096);
    QFile::remove(fetched);
}

// Whatever the cap is set to, it must be the cap. Parameterised rather than
// pinned to a number: the worker count is chosen at runtime from the backend's
// channel count, so a test asserting a literal would break every time that
// tuning changed -- which is exactly what it did when SMB gained parallel reads.
class FetcherConcurrencyTest : public ::testing::TestWithParam<int> {};

TEST_P(FetcherConcurrencyTest, NeverRunsMoreFetchesAtOnceThanTheConfiguredCap) {
    const int cap = GetParam();
    // Each read stalls, so any job allowed to run in parallel beyond the cap
    // would show up in the peak-overlap counter.
    auto provider = std::make_shared<FakeProvider>(payload(256 * 1024), /*readDelayMs=*/20);
    RemoteThumbnailFetcher fetcher;
    fetcher.setMaxConcurrent(cap);
    ASSERT_EQ(fetcher.maxConcurrent(), cap);

    std::atomic<int> finished{0};
    int accepted = 0;
    for (int i = 0; i < 8; ++i) {
        const bool ok = fetcher.submit(provider, [&](const RemoteThumbnailFetcher::Ticket &ticket) {
            const QString p = ticket.download(QStringLiteral("/share/f.jpg"), 256 * 1024);
            if (!p.isEmpty())
                QFile::remove(p);
            ++finished;
        });
        if (ok)
            ++accepted;
    }
    ASSERT_GT(accepted, cap) << "the backlog must hold more than the worker count";
    ASSERT_TRUE(waitFor([&] { return finished.load() == accepted; }, 20000));

    EXPECT_LE(provider->peakConcurrent(), cap)
        << "saw " << provider->peakConcurrent() << " concurrent reads with a cap of " << cap;
}

INSTANTIATE_TEST_SUITE_P(Caps, FetcherConcurrencyTest, ::testing::Values(1, 2, 4));

TEST(RemoteThumbnailFetcherTest, DefaultsToTwoWorkersAndClampsWhatItIsGiven) {
    // Two by default: a backend with one read channel gains nothing from more.
    RemoteThumbnailFetcher fetcher;
    EXPECT_EQ(fetcher.maxConcurrent(), 2);

    // Nonsense values must not disable fetching or open the floodgates.
    fetcher.setMaxConcurrent(0);
    EXPECT_EQ(fetcher.maxConcurrent(), 1);
    fetcher.setMaxConcurrent(-5);
    EXPECT_EQ(fetcher.maxConcurrent(), 1);
    fetcher.setMaxConcurrent(1000);
    EXPECT_EQ(fetcher.maxConcurrent(), 8);
}

TEST(RemoteThumbnailFetcherTest, AdoptsTheWorkerCountAProviderReports) {
    // The wiring that matters: a backend advertising several read channels gets
    // a pool that can use them, without anyone hard-coding the number.
    auto provider = std::make_shared<FakeProvider>(payload(4096), /*readDelayMs=*/0);
    provider->setReadChannels(4);
    RemoteThumbnailFetcher fetcher;
    ASSERT_EQ(fetcher.maxConcurrent(), 2);

    ASSERT_TRUE(fetcher.submit(provider, [](const RemoteThumbnailFetcher::Ticket &ticket) {
        const QString p = ticket.download(QStringLiteral("/share/f.jpg"), 4096);
        if (!p.isEmpty())
            QFile::remove(p);
    }));
    EXPECT_EQ(fetcher.maxConcurrent(), 4);
    waitFor([&] { return fetcher.outstanding() == 0; }, 20000);
}

TEST(RemoteThumbnailFetcherTest, NeverShrinksThePoolForASingleChannelBackend) {
    // A serial backend must not claw back workers another provider is using:
    // shrinking mid-directory would strand already-queued fetches.
    auto wide = std::make_shared<FakeProvider>(payload(4096), /*readDelayMs=*/0);
    wide->setReadChannels(4);
    auto narrow = std::make_shared<FakeProvider>(payload(4096), /*readDelayMs=*/0);
    narrow->setReadChannels(1);

    RemoteThumbnailFetcher fetcher;
    auto noop = [](const RemoteThumbnailFetcher::Ticket &ticket) {
        const QString p = ticket.download(QStringLiteral("/share/f.jpg"), 4096);
        if (!p.isEmpty())
            QFile::remove(p);
    };
    ASSERT_TRUE(fetcher.submit(wide, noop));
    ASSERT_EQ(fetcher.maxConcurrent(), 4);
    ASSERT_TRUE(fetcher.submit(narrow, noop));
    EXPECT_EQ(fetcher.maxConcurrent(), 4);
    waitFor([&] { return fetcher.outstanding() == 0; }, 20000);
}

TEST(RemoteThumbnailFetcherTest, ThumbnailsYieldToVideoPlaybackAndRecoverAfterIt) {
    // A streamed video and the thumbnail grid pull on the same backend, and on
    // SMB that contention is real. Playback is what the user is watching, so
    // fetches drop to a single worker while it runs -- and take their width
    // back once it stops, without anyone having to tell them.
    auto provider = std::make_shared<FakeProvider>(payload(4096), /*readDelayMs=*/0);
    provider->setReadChannels(4);
    RemoteThumbnailFetcher fetcher;
    auto noop = [](const RemoteThumbnailFetcher::Ticket &ticket) {
        const QString p = ticket.download(QStringLiteral("/share/f.jpg"), 4096);
        if (!p.isEmpty())
            QFile::remove(p);
    };

    ASSERT_TRUE(fetcher.submit(provider, noop));
    ASSERT_EQ(fetcher.maxConcurrent(), 4);
    waitFor([&] { return fetcher.outstanding() == 0; }, 20000);

    {
        // Opening a stream is what a video preview does; nothing needs to be
        // read from it for the contention to exist.
        MpvStreamSource::Stream playing(provider, QStringLiteral("/share/clip.mp4"));
        ASSERT_TRUE(fetcher.submit(provider, noop));
        EXPECT_EQ(fetcher.maxConcurrent(), 1) << "thumbnails should stand aside while a clip plays";
        waitFor([&] { return fetcher.outstanding() == 0; }, 20000);
    }

    ASSERT_TRUE(fetcher.submit(provider, noop));
    EXPECT_EQ(fetcher.maxConcurrent(), 4) << "the pool must widen again once playback stops";
    waitFor([&] { return fetcher.outstanding() == 0; }, 20000);
}

TEST(RemoteThumbnailFetcherTest, RefusesWorkOnceTheBacklogIsFull) {
    auto provider = std::make_shared<FakeProvider>(payload(64 * 1024), /*readDelayMs=*/50);
    RemoteThumbnailFetcher fetcher;

    // Far more requests than a viewport's worth: the excess must be refused so
    // the caller re-asks later, rather than queued into an unbounded backlog.
    int accepted = 0, refused = 0;
    for (int i = 0; i < 200; ++i) {
        const bool ok = fetcher.submit(provider, [](const RemoteThumbnailFetcher::Ticket &ticket) {
            const QString p = ticket.download(QStringLiteral("/share/f.jpg"), 64 * 1024);
            if (!p.isEmpty())
                QFile::remove(p);
        });
        ok ? ++accepted : ++refused;
    }
    EXPECT_GT(refused, 0) << "an unbounded queue would have accepted all 200";
    EXPECT_LE(accepted, 200);

    fetcher.cancel(provider.get()); // let the destructor drain quickly
    waitFor([&] { return fetcher.outstanding() == 0; }, 20000);
}

TEST(RemoteThumbnailFetcherTest, CancelStopsAnInFlightDownloadEarly) {
    // Big file, slow reads: without cancellation this job would run for many
    // seconds, so a prompt finish is only possible if the flag is honoured.
    auto provider = std::make_shared<FakeProvider>(payload(4 * 1024 * 1024), /*readDelayMs=*/30);
    RemoteThumbnailFetcher fetcher;

    std::atomic<bool> started{false}, done{false};
    QString fetched;
    ASSERT_TRUE(fetcher.submit(provider, [&](const RemoteThumbnailFetcher::Ticket &ticket) {
        started = true;
        fetched = ticket.download(QStringLiteral("/share/movie.mp4"), 4 * 1024 * 1024);
        done = true;
    }));
    ASSERT_TRUE(waitFor([&] { return started.load(); }, 5000));

    fetcher.cancel(provider.get());
    ASSERT_TRUE(waitFor([&] { return done.load(); }, 5000))
        << "cancelled download kept running";

    // A cancelled fetch yields nothing and leaves no partial file behind.
    EXPECT_TRUE(fetched.isEmpty());
    EXPECT_LT(provider->bytesServed(), 4 * 1024 * 1024);
}

TEST(RemoteThumbnailFetcherTest, CancelOnlyAffectsTheNamedProvider) {
    auto victim = std::make_shared<FakeProvider>(payload(4096));
    auto bystander = std::make_shared<FakeProvider>(payload(4096));
    RemoteThumbnailFetcher fetcher;

    std::atomic<bool> done{false};
    std::atomic<bool> sawCancel{true};
    fetcher.cancel(victim.get()); // bump the victim's epoch before anything runs

    ASSERT_TRUE(fetcher.submit(bystander, [&](const RemoteThumbnailFetcher::Ticket &ticket) {
        sawCancel = ticket.cancelled();
        done = true;
    }));
    ASSERT_TRUE(waitFor([&] { return done.load(); }, 5000));

    // Cancelling one panel's connection must not disturb the other panel's.
    EXPECT_FALSE(sawCancel.load());
}

TEST(RemoteThumbnailFetcherTest, RefusesBackendsThatCannotStream) {
    auto provider = std::make_shared<NoStreamProvider>();
    RemoteThumbnailFetcher fetcher;

    EXPECT_FALSE(fetcher.submit(provider, [](const RemoteThumbnailFetcher::Ticket &) {}));
    EXPECT_EQ(fetcher.outstanding(), 0);
    EXPECT_EQ(provider->opens(), 0);
}

TEST(RemoteThumbnailFetcherTest, DestructorDoesNotBlockOnASlowFetch) {
    auto provider = std::make_shared<FakeProvider>(payload(8 * 1024 * 1024), /*readDelayMs=*/30);
    std::atomic<bool> started{false};

    QElapsedTimer teardown;
    {
        RemoteThumbnailFetcher fetcher;
        ASSERT_TRUE(fetcher.submit(provider, [&](const RemoteThumbnailFetcher::Ticket &ticket) {
            started = true;
            const QString p = ticket.download(QStringLiteral("/share/movie.mp4"), 8 * 1024 * 1024);
            if (!p.isEmpty())
                QFile::remove(p);
        }));
        ASSERT_TRUE(waitFor([&] { return started.load(); }, 5000));
        teardown.start();
    } // ~RemoteThumbnailFetcher: the shutdown flag must cut the fetch short

    EXPECT_LT(teardown.elapsed(), 1000)
        << "teardown waited " << teardown.elapsed() << "ms for a running fetch";
}
