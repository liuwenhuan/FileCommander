#include <gtest/gtest.h>

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include "FileProvider.h"
#include "ThumbnailCache.h"

#include <atomic>

// A remote video thumbnail is taken from a *sparse excerpt* -- only some byte
// ranges of the file are fetched, each written at its true offset, the rest
// left as holes. When the frame grab's seek lands in one of those holes ffmpeg
// does not fail: it complains on stderr, writes a flat grey PNG, and exits 0.
// The old success test (exit 0 + non-empty file) accepted exactly that, so a
// grey rectangle got cached as the file's thumbnail permanently.
//
// These tests build both sides of that distinction with real ffmpeg output --
// a genuinely holed excerpt, and genuinely flat but undamaged videos -- because
// the whole difficulty is that the two look identical by pixel statistics
// alone. Measured: the grey frame stands at stdev 3.1 while a legitimate black
// frame is 0.0, i.e. FLATTER. Any pure variance threshold gets this backwards.
namespace {

bool haveFfmpeg() {
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty() &&
           !QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty();
}

// Renders a short video of one solid colour -- the legitimate low-variance
// case (a black open, a white flash, a solid transition).
bool makeSolidVideo(const QString &path, const QString &colour) {
    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"),
               {QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                QStringLiteral("-i"),
                QStringLiteral("color=c=%1:s=320x240:d=3:r=10").arg(colour),
                QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
                QStringLiteral("yuv420p"), path});
    return proc.waitForFinished(30000) && proc.exitCode() == 0 && QFileInfo(path).size() > 0;
}

// Renders a video with real, varied content.
bool makeBusyVideo(const QString &path) {
    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"),
               {QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                QStringLiteral("-i"), QStringLiteral("testsrc=s=320x240:d=6:r=10"),
                QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
                QStringLiteral("yuv420p"), path});
    return proc.waitForFinished(30000) && proc.exitCode() == 0 && QFileInfo(path).size() > 0;
}

// Renders the shape that actually reproduces the grey screen: HEVC with a
// single keyframe at the very start. Both properties are load-bearing and were
// found by measurement, not assumed --
//   * H.264 conceals aggressively. The same holed excerpt that greys out in
//     HEVC comes back at stdev 83 in H.264, i.e. a perfectly good picture,
//     so an H.264 fixture cannot exhibit the bug at all.
//   * A short GOP lets the decoder resynchronise at the next keyframe inside
//     the fetched tail. With one keyframe at the start, a seek into the hole
//     has nothing to recover from -- which is exactly the real-world case,
//     a long camera clip whose middle was never fetched.
// Needs enough bitrate that 1/16th of the file is nowhere near the seek point.
bool makeLongGopHevcVideo(const QString &path) {
    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"),
               {QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                QStringLiteral("-i"), QStringLiteral("testsrc2=s=640x480:d=25:r=25"),
                QStringLiteral("-c:v"), QStringLiteral("libx265"), QStringLiteral("-pix_fmt"),
                QStringLiteral("yuv420p"), QStringLiteral("-x265-params"),
                QStringLiteral("log-level=none:keyint=250:min-keyint=250:scenecut=0"),
                QStringLiteral("-b:v"), QStringLiteral("6M"), path});
    return proc.waitForFinished(120000) && proc.exitCode() == 0 && QFileInfo(path).size() > 0;
}

bool haveHevcEncoder() {
    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"), {QStringLiteral("-encoders")});
    if (!proc.waitForFinished(10000))
        return false;
    return QString::fromUtf8(proc.readAllStandardOutput()).contains(QStringLiteral("libx265"));
}

// Copies `source` into `dest` keeping its apparent size but writing only the
// given ranges, leaving holes elsewhere -- exactly what
// RemoteThumbnailFetcher::Ticket::downloadRanges() produces. Truncating instead
// would be a different (and much easier) failure: the size changes, so ffprobe
// reports a wrong duration and the seek never even reaches the damage.
bool makeSparseCopy(const QString &source, const QString &dest,
                    const QVector<QPair<qint64, qint64>> &ranges) {
    QFile in(source);
    QFile out(dest);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly))
        return false;
    const qint64 total = in.size();
    if (!out.resize(total)) // apparent size == the original's
        return false;
    for (const auto &range : ranges) {
        const qint64 offset = range.first;
        const qint64 length = qMin(range.second, total - offset);
        if (offset < 0 || length <= 0 || offset >= total)
            continue;
        if (!in.seek(offset) || !out.seek(offset))
            return false;
        const QByteArray chunk = in.read(length);
        if (out.write(chunk) != chunk.size())
            return false;
    }
    return true;
}

struct ThumbnailAttempt {
    bool completed = false;
    bool accepted = false;
};

// The production frame grab, reached through the public cache API. Completion
// is delivered to the cache's GUI-thread slot, so this must pump the event loop
// while waiting; sleeping alone only observes an artificial cache miss.
ThumbnailAttempt thumbnailAttempt(const QString &videoPath) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    QSignalSpy ready(&cache, &ThumbnailCache::thumbnailReady);
    cache.thumbnail(videoPath, 64);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (const QList<QVariant> &arguments : ready) {
            if (arguments.at(0).toString() != videoPath)
                continue;
            return {true, !cache.thumbnail(videoPath, 64).isNull()};
        }
        QTest::qWait(25);
    }
    return {};
}

#ifdef Q_OS_WIN
class NoShellVideoProvider final : public FileProvider {
public:
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
        return nullptr;
    }
    int openCount() const { return m_openCount.load(); }

private:
    std::atomic<int> m_openCount{0};
};

ThumbnailAttempt remoteThumbnailAttempt(const std::shared_ptr<FileProvider> &provider,
                                        const QString &path, qint64 fileSize) {
    ThumbnailCache &cache = ThumbnailCache::instance();
    QSignalSpy ready(&cache, &ThumbnailCache::thumbnailReady);
    const QString connectionId = QStringLiteral("wmf-test-%1")
                                     .arg(QDateTime::currentMSecsSinceEpoch());
    if (cache.requestRemoteThumbnail(provider, connectionId, path,
                                     QDateTime::currentSecsSinceEpoch(), fileSize, 64) !=
        ThumbnailCache::Request::Queued) {
        return {};
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (const QList<QVariant> &arguments : ready) {
            if (arguments.at(0).toString() == path)
                return {true, false};
        }
        QTest::qWait(25);
    }
    return {};
}
#endif

} // namespace

// The regression this whole change exists for. Without the grey-screen check
// this passes -- ffmpeg exits 0 and writes a real PNG -- and the user gets a
// grey rectangle cached forever.
TEST(VideoFrameQualityTest, RejectsAFrameDecodedFromAHoleInTheExcerpt) {
#ifdef Q_OS_WIN
    // Windows never feeds a sparse excerpt to a decoder. The Shell can only
    // open a provider's native path, so a remote video without one must finish
    // as a miss rather than falling back to a partially downloaded temp file.
    const auto provider = std::make_shared<NoShellVideoProvider>();
    const ThumbnailAttempt attempt = remoteThumbnailAttempt(
        provider, QStringLiteral("/quality-fixture/sparse-hole.mp4"), 32LL * 1024 * 1024);
    ASSERT_TRUE(attempt.completed) << "remote video rejection did not complete";
    EXPECT_FALSE(attempt.accepted)
        << "a sparse remote video without a Shell path was handed to a decoder";
    EXPECT_EQ(provider->openCount(), 0)
        << "a remote video without a Shell path fell back to partial downloading";
#else
    if (!haveFfmpeg())
        GTEST_SKIP() << "ffmpeg/ffprobe not installed";

    if (!haveHevcEncoder())
        GTEST_SKIP() << "libx265 not available; cannot build a fixture that greys out";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(makeLongGopHevcVideo(source));

    const qint64 size = QFileInfo(source).size();
    ASSERT_GT(size, 1 << 20) << "need a file big enough to leave a real hole";

    // Head and tail only: the container and its index are present -- so the
    // duration reads correctly and the seek is computed exactly as production
    // computes it -- but the bytes at the 10% seek point were never fetched.
    const QString holed = dir.filePath(QStringLiteral("holed.mp4"));
    const qint64 edge = size / 16;
    ASSERT_TRUE(makeSparseCopy(source, holed, {{0, edge}, {size - edge, edge}}));
    ASSERT_EQ(QFileInfo(holed).size(), size) << "sparse copy must keep the apparent size";

    const ThumbnailAttempt attempt = thumbnailAttempt(holed);
    ASSERT_TRUE(attempt.completed) << "thumbnail generation did not complete";
    EXPECT_FALSE(attempt.accepted)
        << "a grey frame decoded from a hole was accepted as a thumbnail";
#endif
}

// The other half of the contract, and the reason the check is not a plain
// variance threshold: these frames are flatter than the grey one but are real
// content. Rejecting them would mean the file never shows a thumbnail at all.
TEST(VideoFrameQualityTest, AcceptsLegitimatelyFlatFrames) {
    if (!haveFfmpeg())
        GTEST_SKIP() << "ffmpeg/ffprobe not installed";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    struct Case {
        const char *name;
        const char *colour;
    };
    for (const Case &c : {Case{"black.mp4", "black"}, Case{"white.mp4", "white"},
                          Case{"navy.mp4", "0x1a1a2e"}}) {
        const QString path = dir.filePath(QString::fromLatin1(c.name));
        ASSERT_TRUE(makeSolidVideo(path, QString::fromLatin1(c.colour))) << c.name;
        const ThumbnailAttempt attempt = thumbnailAttempt(path);
        ASSERT_TRUE(attempt.completed) << c.name << ": thumbnail generation did not complete";
        EXPECT_TRUE(attempt.accepted)
            << c.name << ": a legitimately single-colour frame was rejected as a grey screen";
    }
}

// An intact video with ordinary content must be unaffected -- the guard only
// ever fires for a frame ffmpeg complained about.
TEST(VideoFrameQualityTest, AcceptsAnIntactVideo) {
    if (!haveFfmpeg())
        GTEST_SKIP() << "ffmpeg/ffprobe not installed";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("intact.mp4"));
    ASSERT_TRUE(makeBusyVideo(path));

    const ThumbnailAttempt attempt = thumbnailAttempt(path);
    ASSERT_TRUE(attempt.completed) << "thumbnail generation did not complete";
    EXPECT_TRUE(attempt.accepted) << "an intact video lost its thumbnail";
}
