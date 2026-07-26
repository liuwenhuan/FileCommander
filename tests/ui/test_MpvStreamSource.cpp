#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <mpv/client.h>

#include <chrono>
#include <clocale>
#include <iostream>
#include <memory>
#include <thread>

#include "FileProvider.h"
#include "MpvStreamSource.h"

// Cover for playing a remote file without downloading it.
//
// The guarantee worth pinning down is a negative one -- that the bytes pulled
// off the backend stay a small fraction of the file -- so every provider here
// counts what it hands over. Nothing is mocked below the FileProvider boundary:
// these are the same openRead/read/seek/closeHandle calls a real SMB or WebDAV
// tab makes.
//
// The tests come in two layers. Most drive MpvStreamSource::Stream directly
// with a scripted access pattern, which needs no libmpv and no video, and
// pins the part that is genuinely tricky: the backends disagree about what a
// seek is, and Stream has to paper over that without knowing which one it is
// talking to. The last one runs a real libmpv against a real H.264 file and
// measures what actually gets read, which is the claim the feature is sold on.
namespace {

// Serves a file's bytes through the FileProvider streaming interface, with the
// seek semantics of a chosen backend family, counting everything.
class CountingProvider : public FileProvider {
public:
    enum class Seeking {
        // SMB (smbc_lseek) and SFTP (libssh2_sftp_seek64): an open handle seeks
        // for real, at any time, for free.
        Random,
        // WebDAV and FTP: seek only lands before the transfer starts, where it
        // becomes an HTTP Range / FTP REST. Once bytes have flowed it refuses,
        // and the only way to the offset is a fresh handle.
        Restart,
    };

    CountingProvider(QString path, Seeking seeking)
        : m_path(std::move(path)), m_seeking(seeking) {}

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
        auto handle = std::make_unique<Handle>();
        handle->file.setFileName(m_path);
        if (!handle->file.open(QIODevice::ReadOnly))
            return nullptr;
        ++m_opens;
        return handle.release();
    }

    qint64 read(FileHandle *h, char *buffer, qint64 maxSize) override {
        auto *handle = static_cast<Handle *>(h);
        if (!handle)
            return -1;
        const qint64 n = handle->file.read(buffer, maxSize);
        if (n > 0) {
            handle->started = true;
            m_bytes += n;
            ++m_reads;
        }
        return n;
    }

    bool seek(FileHandle *h, qint64 offset) override {
        auto *handle = static_cast<Handle *>(h);
        if (!handle)
            return false;
        if (m_seeking == Seeking::Restart && handle->started)
            return false; // the transfer is already running; too late to range it
        return handle->file.seek(offset);
    }

    qint64 handleSize(FileHandle *h) override {
        auto *handle = static_cast<Handle *>(h);
        return handle ? handle->file.size() : -1;
    }

    void closeHandle(FileHandle *h) override { delete h; }

    qint64 bytesRead() const { return m_bytes; }
    int opens() const { return m_opens; }
    int reads() const { return m_reads; }

private:
    struct Handle : public FileHandle {
        QFile file;
        bool started = false;
    };

    QString m_path;
    Seeking m_seeking;
    qint64 m_bytes = 0;
    int m_opens = 0;
    int m_reads = 0;
};

// A file of non-repeating bytes, so a misplaced read shows up as wrong data
// rather than accidentally matching.
QString writePattern(const QDir &dir, const QString &name, qint64 size) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    QByteArray chunk;
    chunk.resize(64 * 1024);
    qint64 written = 0;
    while (written < size) {
        for (int i = 0; i < chunk.size(); ++i)
            chunk[i] = static_cast<char>((written + i) * 31 + (written >> 16));
        const qint64 want = qMin<qint64>(chunk.size(), size - written);
        file.write(chunk.constData(), want);
        written += want;
    }
    file.close();
    return path;
}

std::shared_ptr<CountingProvider> makeProvider(const QString &path,
                                               CountingProvider::Seeking seeking) {
    return std::make_shared<CountingProvider>(path, seeking);
}

qint64 drain(MpvStreamSource::Stream &stream, qint64 bytes, qint64 chunk) {
    QByteArray buffer;
    buffer.resize(static_cast<int>(chunk));
    qint64 got = 0;
    while (got < bytes) {
        const qint64 n = stream.read(buffer.data(), qMin<qint64>(chunk, bytes - got));
        if (n <= 0)
            break;
        got += n;
    }
    return got;
}

bool haveFfmpeg() {
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
}

bool runFfmpeg(const QStringList &args) {
    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"), args);
    proc.waitForFinished(180000);
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// Builds a real H.264/AAC MP4 big enough that "only a fraction was read" means
// something. Encoding to a target size is unreliable -- a synthetic pattern
// compresses down to almost nothing whatever bitrate is asked for -- so this
// encodes one minute and then loops it by stream copy, which costs no encoding
// time and gives a size that is simply a multiple of the first pass.
bool makeVideo(const QDir &dir, const QString &path, int loops) {
    const QString unit = dir.filePath(QStringLiteral("unit.mp4"));
    if (!runFfmpeg({QStringLiteral("-y"),
                    QStringLiteral("-f"), QStringLiteral("lavfi"),
                    QStringLiteral("-i"),
                    QStringLiteral("testsrc=size=1280x720:rate=30:duration=60"),
                    QStringLiteral("-f"), QStringLiteral("lavfi"),
                    QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=60"),
                    QStringLiteral("-c:v"), QStringLiteral("libx264"),
                    QStringLiteral("-preset"), QStringLiteral("ultrafast"),
                    // Without a bitrate floor the synthetic pattern encodes to
                    // a couple of megabytes and the loops below never add up to
                    // a fixture worth measuring against.
                    QStringLiteral("-b:v"), QStringLiteral("6000k"),
                    QStringLiteral("-c:a"), QStringLiteral("aac"),
                    QStringLiteral("-movflags"), QStringLiteral("+faststart"), unit}))
        return false;
    if (!runFfmpeg({QStringLiteral("-y"), QStringLiteral("-stream_loop"),
                    QString::number(loops - 1), QStringLiteral("-i"), unit,
                    QStringLiteral("-c"), QStringLiteral("copy"),
                    QStringLiteral("-movflags"), QStringLiteral("+faststart"), path}))
        return false;
    return QFileInfo(path).size() > 0;
}

} // namespace

// --- Positioning behaviour (no libmpv, no video) ----------------------------

// libmpv seeks every stream to 0 the moment it opens it, purely to find out
// whether the stream is seekable. If that cost a connection, every single file
// would pay a round trip before its first byte -- so a seek must not touch the
// backend until somebody actually reads.
TEST(MpvStreamSource, SeekAloneNeverTouchesTheBackend) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), 4 << 20);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Random);
    MpvStreamSource::Stream stream(provider, path);

    EXPECT_EQ(stream.seek(0), 0);
    EXPECT_EQ(stream.seek(1234567), 1234567);
    EXPECT_EQ(stream.seek(0), 0);

    EXPECT_EQ(provider->opens(), 0);
    EXPECT_EQ(provider->bytesRead(), 0);
}

// The whole point: reading a megabyte out of the middle of a file costs a
// megabyte, not the file.
TEST(MpvStreamSource, ReadsOnlyWhatWasAskedFor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 fileSize = 8 << 20;
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), fileSize);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Random);
    MpvStreamSource::Stream stream(provider, path);

    EXPECT_EQ(stream.size(), fileSize);
    ASSERT_EQ(stream.seek(4 << 20), 4 << 20);
    EXPECT_EQ(drain(stream, 1 << 20, 256 * 1024), 1 << 20);

    EXPECT_EQ(provider->bytesRead(), 1 << 20);
    EXPECT_EQ(stream.bytesFetched(), 1 << 20);
}

// What comes out has to be the bytes that live at that offset -- a stream that
// silently returns the wrong region would still "play", just garbled.
TEST(MpvStreamSource, DeliversTheBytesThatLiveAtTheOffset) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), 4 << 20);
    ASSERT_FALSE(path.isEmpty());

    QFile truth(path);
    ASSERT_TRUE(truth.open(QIODevice::ReadOnly));
    ASSERT_TRUE(truth.seek(1000003));
    const QByteArray expected = truth.read(40000);

    for (auto seeking : {CountingProvider::Seeking::Random, CountingProvider::Seeking::Restart}) {
        auto provider = makeProvider(path, seeking);
        MpvStreamSource::Stream stream(provider, path);
        // Read a little first, so a Restart backend has genuinely started and
        // the seek below has to re-establish the transfer.
        ASSERT_GT(drain(stream, 128 * 1024, 64 * 1024), 0);
        ASSERT_EQ(stream.seek(1000003), 1000003);

        QByteArray got;
        QByteArray buffer;
        buffer.resize(40000);
        while (got.size() < expected.size()) {
            const qint64 n = stream.read(buffer.data(), expected.size() - got.size());
            ASSERT_GT(n, 0);
            got.append(buffer.constData(), static_cast<int>(n));
        }
        EXPECT_EQ(got, expected);
    }
}

// SMB and SFTP hand out a handle that seeks for real, so a whole session --
// however much it jumps around -- should run on one connection.
TEST(MpvStreamSource, RandomSeekBackendKeepsASingleConnection) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 fileSize = 8 << 20;
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), fileSize);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Random);
    MpvStreamSource::Stream stream(provider, path);

    // The shape libmpv produces on an MP4 with its index at the tail: read the
    // head, jump to the end for the moov atom, come back, then stream.
    ASSERT_GT(drain(stream, 256 * 1024, 128 * 1024), 0);
    ASSERT_EQ(stream.seek(fileSize - 200000), fileSize - 200000);
    ASSERT_GT(drain(stream, 100000, 65536), 0);
    ASSERT_EQ(stream.seek(48), 48);
    ASSERT_GT(drain(stream, 512 * 1024, 256 * 1024), 0);

    EXPECT_EQ(provider->opens(), 1);
    EXPECT_EQ(stream.opens(), 1);
}

// WebDAV and FTP cannot seek a running transfer, so a jump backwards has to
// start a new one -- but only when it really has to.
TEST(MpvStreamSource, RestartBackendReopensOnlyForFarSeeks) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 fileSize = 8 << 20;
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), fileSize);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Restart);
    MpvStreamSource::Stream stream(provider, path);

    ASSERT_GT(drain(stream, 256 * 1024, 128 * 1024), 0);
    EXPECT_EQ(provider->opens(), 1);

    // Backwards: unreachable without a new transfer.
    ASSERT_EQ(stream.seek(1024), 1024);
    ASSERT_GT(drain(stream, 128 * 1024, 65536), 0);
    EXPECT_EQ(provider->opens(), 2);
}

// A small gap forward is cheaper to read through and throw away than to pay a
// connection setup plus a round trip for. The skipped bytes still crossed the
// wire, so they must be counted, not hidden.
TEST(MpvStreamSource, ShortForwardGapIsSkippedInsteadOfReconnecting) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), 8 << 20);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Restart);
    MpvStreamSource::Stream stream(provider, path);

    ASSERT_GT(drain(stream, 256 * 1024, 128 * 1024), 0);
    const qint64 before = provider->bytesRead();

    // Half a megabyte forward: inside the skip budget.
    ASSERT_EQ(stream.seek(256 * 1024 + 512 * 1024), 256 * 1024 + 512 * 1024);
    ASSERT_GT(drain(stream, 128 * 1024, 128 * 1024), 0);

    EXPECT_EQ(provider->opens(), 1) << "a short forward hop should not reconnect";
    EXPECT_GE(provider->bytesRead(), before + 512 * 1024)
        << "bytes read through to reach the offset must still be counted";
}

// A far seek forward is not worth reading through: past the budget the stream
// should reconnect rather than drag the gap across the wire.
TEST(MpvStreamSource, LongForwardGapReconnectsRatherThanReadingThrough) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 fileSize = 32 << 20;
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), fileSize);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Restart);
    MpvStreamSource::Stream stream(provider, path);

    ASSERT_GT(drain(stream, 256 * 1024, 128 * 1024), 0);
    const qint64 before = provider->bytesRead();

    ASSERT_EQ(stream.seek(24 << 20), 24 << 20);
    ASSERT_GT(drain(stream, 128 * 1024, 128 * 1024), 0);

    EXPECT_EQ(provider->opens(), 2);
    EXPECT_LT(provider->bytesRead() - before, 2 << 20)
        << "a 24 MB gap must not be read through";
}

// A container with no index -- MPEG-TS above all -- gives the demuxer no way to
// turn a timestamp into an offset, so it bisects the file with a cluster of
// tiny reads. Measured with a real libmpv on a 141 MB .ts, seeking to the 90%
// mark issued 23 seeks, and eight of those probes landed within the same 18 KB.
// On a backend that cannot seek a live transfer, each one would otherwise be a
// separate HTTP request; the block cache is what stops that.
TEST(MpvStreamSource, ClusteredProbesCollapseIntoFewFetches) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 fileSize = 32 << 20;
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), fileSize);
    ASSERT_FALSE(path.isEmpty());

    // The offsets libmpv actually probed, rebased onto this file's size so the
    // spacing -- and the tight cluster in the middle of it -- is preserved.
    const QVector<qint64> probes = {
        141189504, 141187436, 141183300, 141175216, 141158860, 127068260, 127051876,
        63527268,  127053220, 95291936,  127052468, 111173236, 127051904, 119114168,
        127051340, 123085292, 127050588, 125070384, 127050024};
    const qint64 measuredSize = 141190444;

    auto provider = makeProvider(path, CountingProvider::Seeking::Restart);
    MpvStreamSource::Stream stream(provider, path);

    QByteArray buffer;
    buffer.resize(12 * 1024); // the probe size libmpv used
    for (qint64 probe : probes) {
        const qint64 offset = probe * fileSize / measuredSize;
        ASSERT_EQ(stream.seek(offset), offset);
        ASSERT_GT(stream.read(buffer.data(), buffer.size()), 0);
    }

    // Without the cache every probe is its own transfer. The cluster alone
    // should save several.
    EXPECT_LT(provider->opens(), probes.size())
        << "clustered probes should share a fetch, not reconnect one by one";
    EXPECT_LE(provider->opens(), 14);
}

// Cancellation comes from a different thread than the reads, and libmpv relies
// on it to break out of a stream whose backend has gone quiet.
TEST(MpvStreamSource, CancelStopsFurtherReads) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), 4 << 20);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Random);
    MpvStreamSource::Stream stream(provider, path);

    QByteArray buffer;
    buffer.resize(256 * 1024);
    ASSERT_GT(stream.read(buffer.data(), buffer.size()), 0);

    stream.cancel();
    EXPECT_EQ(stream.read(buffer.data(), buffer.size()), -1);
    const qint64 after = provider->bytesRead();
    EXPECT_EQ(stream.read(buffer.data(), buffer.size()), -1);
    EXPECT_EQ(provider->bytesRead(), after) << "a cancelled stream must not keep fetching";
}

// --- Publication ------------------------------------------------------------

TEST(MpvStreamSource, PublishedUrlIsRecognisedAndKeepsTheExtension) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), 1024);
    ASSERT_FALSE(path.isEmpty());

    auto provider = makeProvider(path, CountingProvider::Seeking::Random);
    const QString url = MpvStreamSource::publish(provider, QStringLiteral("/share/Some Clip.mkv"));

    ASSERT_FALSE(url.isEmpty());
    EXPECT_TRUE(MpvStreamSource::isStreamUrl(url));
    EXPECT_FALSE(MpvStreamSource::isStreamUrl(QStringLiteral("/tmp/local.mkv")));
    // The suffix is what tells the preview -- and mpv -- what the format is.
    EXPECT_EQ(QFileInfo(url).suffix(), QStringLiteral("mkv"));
    // A space must not end the URL early.
    EXPECT_FALSE(url.contains(QLatin1Char(' ')));

    MpvStreamSource::revoke(url);
}

// A playing stream holds a read channel for its whole life, and
// RemoteThumbnailFetcher sizes its worker pool by subtracting these -- so the
// count has to be exact, and has to come back down.
TEST(MpvStreamSource, CountsOpenStreamsSoThumbnailsDoNotOversubscribe) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePattern(QDir(dir.path()), QStringLiteral("p.bin"), 1 << 20);
    ASSERT_FALSE(path.isEmpty());
    auto provider = makeProvider(path, CountingProvider::Seeking::Random);

    const int before = MpvStreamSource::activeStreams();
    {
        MpvStreamSource::Stream one(provider, path);
        EXPECT_EQ(MpvStreamSource::activeStreams(), before + 1);
        {
            MpvStreamSource::Stream two(provider, path);
            EXPECT_EQ(MpvStreamSource::activeStreams(), before + 2);
        }
        EXPECT_EQ(MpvStreamSource::activeStreams(), before + 1);
    }
    EXPECT_EQ(MpvStreamSource::activeStreams(), before);
}

TEST(MpvStreamSource, RefusesBackendsThatCannotStream) {
    class NoStream : public CountingProvider {
    public:
        NoStream() : CountingProvider(QString(), Seeking::Random) {}
        bool canStream() const override { return false; }
    };
    EXPECT_TRUE(MpvStreamSource::publish(std::make_shared<NoStream>(),
                                         QStringLiteral("/share/x.mp4"))
                    .isEmpty());
}

// --- End to end, with a real libmpv and a real video ------------------------

// The claim this whole path is sold on: playing a remote video reads a small
// fraction of it, instead of the download it used to be. Everything here is
// real -- a real H.264 file, a real mpv core decoding it, and a byte count
// taken at the FileProvider boundary the same way an SMB tab would go through.
TEST(MpvStreamSource, PlayingThroughLibmpvReadsAFractionOfTheFile) {
    if (!haveFfmpeg())
        GTEST_SKIP() << "ffmpeg not installed; cannot build a video fixture";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString video = QDir(dir.path()).filePath(QStringLiteral("clip.mp4"));
    if (!makeVideo(QDir(dir.path()), video, 6))
        GTEST_SKIP() << "ffmpeg could not encode the fixture (no libx264?)";

    const qint64 fileSize = QFileInfo(video).size();
    // A fraction only means something against a file big enough that reading it
    // whole would be obvious. How well the encoder compresses the pattern is an
    // environment property, so fall out with a reason rather than fail.
    if (fileSize < (32 << 20))
        GTEST_SKIP() << "fixture came out at " << fileSize
                     << " bytes; too small to distinguish streaming from downloading";

    auto provider = makeProvider(video, CountingProvider::Seeking::Random);
    const QString url = MpvStreamSource::publish(provider, video);
    ASSERT_FALSE(url.isEmpty());

    // libmpv refuses to create a context unless LC_NUMERIC is "C", and
    // QApplication sets the locale from the environment during construction.
    // src/main.cpp does exactly this for the same reason.
    std::setlocale(LC_NUMERIC, "C");

    mpv_handle *mpv = mpv_create();
    ASSERT_NE(mpv, nullptr);
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "vo", "null");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "mute", "yes");
    // The same bounds MpvWidget applies; without them mpv reads ahead 150 MB
    // and the measurement below would be of the default, not of this feature.
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "8MiB");
    ASSERT_GE(mpv_initialize(mpv), 0);
    ASSERT_TRUE(MpvStreamSource::registerProtocol(mpv));

    const QByteArray urlUtf8 = url.toUtf8();
    const char *cmd[] = {"loadfile", urlUtf8.constData(), nullptr};
    ASSERT_GE(mpv_command(mpv, cmd), 0);

    // Let it open and play. mpv decodes on its own clock, so this is wall time.
    bool loaded = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        mpv_event *ev = mpv_wait_event(mpv, 0.1);
        if (!ev)
            continue;
        if (ev->event_id == MPV_EVENT_FILE_LOADED)
            loaded = true;
        if (ev->event_id == MPV_EVENT_END_FILE) {
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            ASSERT_NE(ef->reason, MPV_END_FILE_REASON_ERROR)
                << "the stream failed to open: " << mpv_error_string(ef->error);
            break;
        }
        double pos = 0;
        if (loaded && mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0 &&
            pos >= 3.0)
            break;
    }
    ASSERT_TRUE(loaded) << "libmpv never opened the stream";

    // It really decoded this file, rather than opening something empty.
    double duration = 0;
    ASSERT_GE(mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration), 0);
    EXPECT_NEAR(duration, 360.0, 5.0);

    const qint64 played = provider->bytesRead();

    // Seeking to the far end must not drag the file across either: it is a jump
    // to an offset, not a fast-forward through everything in between.
    const char *seekCmd[] = {"seek", "324", "absolute", nullptr};
    ASSERT_GE(mpv_command(mpv, seekCmd), 0);
    const auto seekDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < seekDeadline) {
        mpv_wait_event(mpv, 0.1);
        double pos = 0;
        if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0 && pos >= 326.0)
            break;
    }

    const qint64 total = provider->bytesRead();
    const int opens = provider->opens();

    // Guarantees the streams close before the provider is inspected.
    mpv_terminate_destroy(mpv);
    MpvStreamSource::revoke(url);

    // The measurement is the point of this test, so say it out loud rather than
    // only on failure.
    std::cout << "[  STREAM  ] " << fileSize << " byte file: " << played << " bytes ("
              << (100.0 * played / fileSize) << "%) to start playing, " << total
              << " bytes (" << (100.0 * total / fileSize) << "%) after seeking to 90%, "
              << opens << " connection(s)" << std::endl;

    // Generous on purpose: the exact figure moves with the encode and with
    // mpv's readahead, and a test that fails on a 2% drift would just get
    // muted. Downloading would be 100%; measured here it lands near 20%.
    EXPECT_LT(played, fileSize / 3)
        << "starting playback read " << played << " of " << fileSize << " bytes";
    EXPECT_LT(total, fileSize / 2)
        << "playback plus a seek to the end read " << total << " of " << fileSize << " bytes";
    // A backend that seeks for real should never have needed a second handle.
    EXPECT_EQ(opens, 1);
}
