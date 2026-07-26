#include <gtest/gtest.h>

#include <QElapsedTimer>

#include "SmbHelperClient.h"
#include "SmbHelperProtocol.h"

// The wire format is the contract between two separately-compiled binaries (FileCommander
// and FileCommander-smb-helper), so its codecs are worth pinning down: a silent change to
// either side's byte order would desynchronise the stream rather than fail
// loudly.
using namespace smbhelper;

TEST(SmbHelperProtocol, FixedWidthValuesSurviveARoundTrip) {
    unsigned char buf[8];

    putU32(buf, 0);
    EXPECT_EQ(getU32(buf), 0u);
    putU32(buf, 0xDEADBEEFu);
    EXPECT_EQ(getU32(buf), 0xDEADBEEFu);
    putU32(buf, 0xFFFFFFFFu);
    EXPECT_EQ(getU32(buf), 0xFFFFFFFFu);

    // A file offset past 4 GiB is the case that matters: the share this was
    // built for holds multi-gigabyte videos, and a truncated seek offset would
    // read the wrong bytes rather than fail.
    putU64(buf, 0);
    EXPECT_EQ(getU64(buf), 0ull);
    putU64(buf, 0x1234567890ABCDEFull);
    EXPECT_EQ(getU64(buf), 0x1234567890ABCDEFull);
    putU64(buf, 5ull * 1024 * 1024 * 1024);
    EXPECT_EQ(getU64(buf), 5ull * 1024 * 1024 * 1024);
}

TEST(SmbHelperProtocol, EncodingIsLittleEndianRegardlessOfHost) {
    // Spelled out byte by byte: the format must not quietly become whatever the
    // build machine's native order happens to be.
    unsigned char buf[4];
    putU32(buf, 0x01020304u);
    EXPECT_EQ(buf[0], 0x04);
    EXPECT_EQ(buf[1], 0x03);
    EXPECT_EQ(buf[2], 0x02);
    EXPECT_EQ(buf[3], 0x01);
}

TEST(SmbHelperProtocol, HeaderCarriesLengthAndOpcode) {
    unsigned char header[kHeaderSize];
    writeHeader(header, 1024, static_cast<std::uint8_t>(Op::Read));
    EXPECT_EQ(getU32(header), 1024u);
    EXPECT_EQ(header[4], static_cast<std::uint8_t>(Op::Read));
}

TEST(SmbHelperProtocol, ReadChunkFitsInsideTheFrameLimit) {
    // A read reply carries its bytes inline, so the chunk ceiling must stay
    // under the frame ceiling or a full-size read would be rejected by the
    // peer's own bounds check.
    EXPECT_LE(kMaxReadChunk, kMaxPayload);
}

// The helper is optional acceleration: when it is missing, every path must
// degrade to the in-process one rather than fail. available() is what the
// provider relies on for that, so it has to answer honestly.
TEST(SmbHelperClientTest, ReportsAvailabilityWithoutSpawningAnything) {
    // Both answers are legitimate depending on whether the build produced the
    // helper next to the test binary; what matters is that asking is safe and
    // consistent with the resolved path.
    EXPECT_EQ(SmbHelperClient::available(), !SmbHelperClient::helperPath().isEmpty());
}

TEST(SmbHelperClientTest, AcquireYieldsNothingUntilConfigured) {
    // No credentials means no connection to make, so acquire() must refuse
    // rather than spawn a helper that could only fail its handshake.
    SmbHelperClient client;
    EXPECT_EQ(client.acquire(), nullptr);
}

TEST(SmbHelperClientTest, AcquireYieldsNothingAfterShutdown) {
    SmbHelperClient client;
    client.configure(QStringLiteral("198.51.100.1"), QStringLiteral("u"), QStringLiteral("p"),
                     QString(), false, 1000);
    client.shutdown();
    EXPECT_EQ(client.acquire(), nullptr);
}

TEST(SmbHelperClientTest, ShutdownIsIdempotent) {
    // The provider calls shutdown() from both disconnect() and its destructor,
    // so a second call must be harmless rather than double-free a channel.
    SmbHelperClient client;
    client.configure(QStringLiteral("198.51.100.1"), QStringLiteral("u"), QStringLiteral("p"),
                     QString(), false, 1000);
    client.shutdown();
    client.shutdown();
    SUCCEED();
}

TEST(SmbHelperClientTest, AcquireFailsWithoutHangingOnAnUnroutableHost) {
    // 198.51.100.0/24 is TEST-NET-2: reserved for documentation and not
    // routable, so the helper's connect cannot succeed. The point is that
    // acquire() gives up within the configured timeout and hands back nullptr
    // (the caller then uses the in-process path) instead of blocking a
    // thumbnail worker.
    SmbHelperClient client;
    client.configure(QStringLiteral("198.51.100.1"), QStringLiteral("user"),
                     QStringLiteral("pass"), QString(), false, 2000);
    QElapsedTimer timer;
    timer.start();
    SmbHelperClient::Channel *channel = client.acquire();
    const qint64 elapsed = timer.elapsed();
    if (channel)
        client.release(channel);
    EXPECT_EQ(channel, nullptr);
    EXPECT_LT(elapsed, 30000) << "acquire must fail fast, not stall the caller";
}
