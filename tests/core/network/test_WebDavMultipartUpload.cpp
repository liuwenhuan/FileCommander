#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "account/FileShareServer.h"
#include "account/ShareIdentity.h"
#include "network/CurlWebDavProvider.h"

namespace {

constexpr qint64 kPayloadSize = 21LL * 1024 * 1024 + 17;

class WebDavMultipartUploadTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_share = m_dir.filePath(QStringLiteral("share"));
        ASSERT_TRUE(QDir().mkpath(m_share));
        m_source = m_dir.filePath(QStringLiteral("source.bin"));
        QFile file(m_source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray block(64 * 1024, 'M');
        while (file.size() < kPayloadSize) {
            const qint64 remaining = kPayloadSize - file.size();
            ASSERT_EQ(file.write(block.constData(), qMin<qint64>(remaining, block.size())),
                      qMin<qint64>(remaining, block.size()));
        }
        file.close();

        m_server = new FileShareServer(nullptr);
        m_server->setSharedFolders({m_share});
        m_server->addTicket(QStringLiteral("multipart-client-test"), 300);
        QSignalSpy started(m_server, &FileShareServer::started);
        m_server->start();
        ASSERT_TRUE(started.wait(5000));
        m_port = started.first().first().toUInt();
    }

    void TearDown() override { delete m_server; }

    bool connect(CurlWebDavProvider &provider, bool relay) {
        provider.setPinnedPublicKey(ShareIdentity::local().pin);
        provider.setRelayDeviceRoute(relay);
        QString error;
        const bool connected = provider.connectToHost(QStringLiteral("127.0.0.1"), int(m_port),
                                      QStringLiteral("device"),
                                      QStringLiteral("multipart-client-test"), true, &error);
        if (!connected)
            ADD_FAILURE() << error.toStdString();
        return connected;
    }

    QTemporaryDir m_dir;
    QString m_share;
    QString m_source;
    FileShareServer *m_server = nullptr;
    quint16 m_port = 0;
};

TEST_F(WebDavMultipartUploadTest, RelayRouteAndPeerCapabilityAreRequired) {
    CurlWebDavProvider direct;
    ASSERT_TRUE(connect(direct, false));
    EXPECT_FALSE(direct.canUploadLocalFileParallel());

    CurlWebDavProvider relay;
    ASSERT_TRUE(connect(relay, true));
    EXPECT_TRUE(relay.canUploadLocalFileParallel());
}

TEST_F(WebDavMultipartUploadTest, BenchmarkSingleStreamOverrideDisablesParallelUpload) {
    CurlWebDavProvider relay;
    ASSERT_TRUE(connect(relay, true));
    ASSERT_TRUE(relay.canUploadLocalFileParallel());

    const bool hadPrevious = qEnvironmentVariableIsSet("FILECOMMANDER_BENCH_SINGLE_STREAM");
    const QByteArray previous = qgetenv("FILECOMMANDER_BENCH_SINGLE_STREAM");
    qputenv("FILECOMMANDER_BENCH_SINGLE_STREAM", "1");
    EXPECT_FALSE(relay.canUploadLocalFileParallel());
    if (hadPrevious)
        qputenv("FILECOMMANDER_BENCH_SINGLE_STREAM", previous);
    else
        qunsetenv("FILECOMMANDER_BENCH_SINGLE_STREAM");
    EXPECT_TRUE(relay.canUploadLocalFileParallel());
}

TEST_F(WebDavMultipartUploadTest, ThreePartsCommitWithConfirmedProgress) {
    CurlWebDavProvider provider;
    ASSERT_TRUE(connect(provider, true));
    ASSERT_TRUE(provider.canUploadLocalFileParallel());

    qint64 lastReceived = -1;
    qint64 lastSent = -1;
    const CloseHandleResult result = provider.uploadLocalFileParallel(
        m_source, QStringLiteral("/share/target.bin"),
        [&](qint64 sent, qint64 received) {
            EXPECT_GE(received, lastReceived);
            EXPECT_GE(sent, received);
            lastReceived = received;
            lastSent = sent;
            return true;
        },
        [] { return true; }, nullptr);

    EXPECT_TRUE(result.committed) << result.detail.toStdString();
    EXPECT_EQ(lastReceived, kPayloadSize);
    EXPECT_GE(lastSent, lastReceived);
    QFile target(m_share + QStringLiteral("/target.bin"));
    ASSERT_TRUE(target.open(QIODevice::ReadOnly));
    EXPECT_EQ(target.size(), kPayloadSize);
    EXPECT_EQ(target.read(16), QByteArray(16, 'M'));
    EXPECT_TRUE(target.seek(kPayloadSize - 17));
    EXPECT_EQ(target.readAll(), QByteArray(17, 'M'));
}

TEST_F(WebDavMultipartUploadTest, CancelLeavesResumableParts) {
    CurlWebDavProvider provider;
    ASSERT_TRUE(connect(provider, true));
    qint64 confirmedBeforeCancel = 0;
    QString error;
    const CloseHandleResult cancelled = provider.uploadLocalFileParallel(
        m_source, QStringLiteral("/share/resume.bin"),
        [&](qint64, qint64 received) {
            confirmedBeforeCancel = received;
            return received == 0;
        },
        [] { return true; }, &error);
    EXPECT_FALSE(cancelled.committed);
    EXPECT_GT(confirmedBeforeCancel, 0);
    EXPECT_FALSE(QFileInfo::exists(m_share + QStringLiteral("/resume.bin")));
    qint64 persisted = 0;
    for (int i = 0; i < 3; ++i)
        persisted += QFileInfo(m_share + QStringLiteral("/.resume.bin.filecommander-multipart.%1")
                                         .arg(i)).size();
    EXPECT_GT(persisted, 0);

    const CloseHandleResult resumed = provider.uploadLocalFileParallel(
        m_source, QStringLiteral("/share/resume.bin"),
        [](qint64, qint64) { return true; }, [] { return true; }, &error);
    EXPECT_TRUE(resumed.committed) << resumed.detail.toStdString();
    EXPECT_EQ(QFileInfo(m_share + QStringLiteral("/resume.bin")).size(), kPayloadSize);
}

TEST_F(WebDavMultipartUploadTest, CancellationDuringCommitReportsPublishedFile) {
    m_server->setCommitDelayMsForTesting(3000);
    CurlWebDavProvider provider;
    ASSERT_TRUE(connect(provider, true));

    std::atomic<bool> cancelled{false};
    std::atomic<bool> scheduled{false};
    std::thread cancelThread;
    const CloseHandleResult result = provider.uploadLocalFileParallel(
        m_source, QStringLiteral("/share/finalizing.bin"),
        [&](qint64, qint64 received) {
            if (received == kPayloadSize && !scheduled.exchange(true)) {
                cancelThread = std::thread([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    cancelled.store(true);
                });
            }
            return true;
        },
        [&] { return !cancelled.load(); }, nullptr);
    if (cancelThread.joinable())
        cancelThread.join();

    ASSERT_TRUE(scheduled.load());
    ASSERT_TRUE(cancelled.load());
    EXPECT_TRUE(result.committed) << result.detail.toStdString();
    EXPECT_EQ(QFileInfo(m_share + QStringLiteral("/finalizing.bin")).size(), kPayloadSize);
}

} // namespace
