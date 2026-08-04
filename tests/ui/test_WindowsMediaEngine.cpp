#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <utility>

#include "FileProvider.h"
#include "QuickView.h"
#include "config/Settings.h"
#include "media/MediaEngine.h"
#include "media/SeekWatchdog.h"
#include "media/WindowsMediaEngine.h"
#include "media/WindowsMediaSurface.h"

namespace {

class SchemeProvider final : public FileProvider {
public:
    explicit SchemeProvider(QString scheme) : m_scheme(std::move(scheme)) {}

    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &path) const override { return path; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return false; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
    QString scheme() const override { return m_scheme; }

private:
    QString m_scheme;
};

WindowsMediaSurface *findWindowsMediaSurface(QObject *root) {
    if (!root)
        return nullptr;
    if (auto *surface = dynamic_cast<WindowsMediaSurface *>(root))
        return surface;
    for (QObject *child : root->children()) {
        if (auto *surface = findWindowsMediaSurface(child))
            return surface;
    }
    return nullptr;
}

QFileInfo wmfFixture(const QString &name) {
    return QFileInfo(QStringLiteral(TTC_WMF_FIXTURE_DIR "/%1").arg(name));
}

QFileInfo configuredWmfFixture(const QString &name) {
    const QString configured = QString::fromLocal8Bit(qgetenv("FILECOMMANDER_WMF_TEST_VIDEO"));
    return configured.isEmpty() ? wmfFixture(name) : QFileInfo(configured);
}

void expectLocalVideoPlaybackAndFrame(const QString &path) {
    const QFileInfo fixture(path);
    ASSERT_TRUE(fixture.exists()) << path.toStdString();

    WindowsMediaEngine engine;
    QString error;
    QObject::connect(&engine, &MediaEngine::errorOccurred, [&error](const QString &message) {
        error = message;
    });
    auto *surface = dynamic_cast<WindowsMediaSurface *>(engine.videoSurface());
    ASSERT_NE(surface, nullptr);

    MediaSource source;
    source.path = fixture.absoluteFilePath();

    engine.load(source, MediaKind::Video);

    bool sawPlaying = false;
    bool sawFrame = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 7000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(25);
        sawPlaying = sawPlaying || engine.state() == MediaState::Playing;
        sawFrame = sawFrame || !surface->currentFrameForTest().isNull();
        if ((engine.state() == MediaState::Failed) ||
            (sawPlaying && sawFrame && engine.durationSeconds() > 0.0)) {
            break;
        }
    }

    EXPECT_NE(engine.state(), MediaState::Failed) << error.toStdString();
    EXPECT_TRUE(sawPlaying) << error.toStdString();
    EXPECT_GT(engine.durationSeconds(), 0.0);
    EXPECT_TRUE(sawFrame) << error.toStdString();
}

quint64 frameSignature(const QImage &frame) {
    if (frame.isNull())
        return 0;
    const QImage image = frame.convertToFormat(QImage::Format_ARGB32);
    quint64 hash = 1469598103934665603ULL;
    const int stepX = qMax(1, image.width() / 16);
    const int stepY = qMax(1, image.height() / 12);
    for (int y = 0; y < image.height(); y += stepY) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += stepX) {
            hash ^= static_cast<quint64>(line[x]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void waitForMovingVideo(WindowsMediaEngine &engine, WindowsMediaSurface *surface,
                        std::set<quint64> *signatures, QString *error) {
    ASSERT_NE(surface, nullptr);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 7000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(25);
        if (!surface->currentFrameForTest().isNull())
            signatures->insert(frameSignature(surface->currentFrameForTest()));
        if (engine.state() == MediaState::Failed)
            break;
        if (signatures->size() >= 3 && engine.durationSeconds() > 0.0)
            break;
    }
    EXPECT_NE(engine.state(), MediaState::Failed) << error->toStdString();
    EXPECT_GT(engine.durationSeconds(), 0.0);
    EXPECT_GE(signatures->size(), 3u) << error->toStdString();
}

void waitForQuickViewMovingVideo(QuickView &view, WindowsMediaSurface *surface,
                                 std::set<quint64> *signatures, QString *error) {
    ASSERT_NE(surface, nullptr);
    auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 7000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(25);
        if (info && info->text().contains(QStringLiteral("Media Foundation")))
            *error = info->text();
        if (!surface->currentFrameForTest().isNull())
            signatures->insert(frameSignature(surface->currentFrameForTest()));
        if (!error->isEmpty() || signatures->size() >= 3)
            break;
    }
    EXPECT_TRUE(error->isEmpty()) << error->toStdString();
    EXPECT_TRUE(surface->isVisibleTo(&view));
    EXPECT_GE(signatures->size(), 3u) << error->toStdString();
}

} // namespace

TEST(WindowsMediaEngine, WebDavRemoteSourceResolvesToLocalCacheUrlForMediaFoundation) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString cachePath = dir.filePath(QStringLiteral("clip.mp4"));
    QFile cache(cachePath);
    ASSERT_TRUE(cache.open(QIODevice::WriteOnly));
    ASSERT_EQ(cache.write("cached media bytes"), 18);
    cache.close();

    MediaSource source;
    source.path = QStringLiteral("/remote/video.mp4");
    source.provider = std::make_shared<SchemeProvider>(QStringLiteral("webdav"));
    source.isRemote = true;
    source.localCachePath = cachePath;

    const QString url = WindowsMediaEngine::sourceUrlForMediaFoundation(source);

    EXPECT_EQ(url, QUrl::fromLocalFile(cachePath).toString(QUrl::None));
}

TEST(WindowsMediaEngine, SftpAndFtpRemoteSourcesDoNotResolveAsPlayableWindowsMfUrls) {
    for (const QString &scheme :
         {QStringLiteral("sftp"), QStringLiteral("ftp"), QStringLiteral("smb")}) {
        MediaSource source;
        source.path = QStringLiteral("/remote/video.mp4");
        source.provider = std::make_shared<SchemeProvider>(scheme);
        source.isRemote = true;
        source.localCachePath = QStringLiteral("C:/FileCommander/cache/video.mp4");

        EXPECT_TRUE(WindowsMediaEngine::sourceUrlForMediaFoundation(source).isEmpty())
            << scheme.toStdString();
    }
}

TEST(WindowsMediaEngine, NonCachedRemoteProviderSourcesStillFailFast) {
    WindowsMediaEngine engine;
    QSignalSpy states(&engine, &MediaEngine::stateChanged);
    QString error;
    QObject::connect(&engine, &MediaEngine::errorOccurred, [&error](const QString &message) {
        error = message;
    });

    MediaSource source;
    source.path = QStringLiteral("/remote/video.mp4");
    source.provider = std::make_shared<SchemeProvider>(QStringLiteral("webdav"));
    source.isRemote = true;

    engine.load(source, MediaKind::Video);

    EXPECT_EQ(engine.state(), MediaState::Failed);
    EXPECT_TRUE(error.contains(QStringLiteral("local cache"), Qt::CaseInsensitive));
    EXPECT_TRUE(error.contains(source.path));
    ASSERT_FALSE(states.isEmpty());
    EXPECT_EQ(qvariant_cast<MediaState>(states.last().at(0)), MediaState::Failed);
}

TEST(WindowsMediaEngine, LocalH264Mp4ReachesPlaybackAndProducesFrame) {
    const QFileInfo fixture = wmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists()) {
        GTEST_SKIP() << "local WMF fixture is not present";
    }

    expectLocalVideoPlaybackAndFrame(fixture.absoluteFilePath());
}

TEST(WindowsMediaEngine, EnvLocalVideoPathReachesPlaybackAndProducesFrame) {
    const QFileInfo fixture = configuredWmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists())
        GTEST_SKIP() << "configured WMF video and build fixture are both unavailable";

    expectLocalVideoPlaybackAndFrame(fixture.absoluteFilePath());
}

TEST(WindowsMediaEngine, LocalH264Mp4KeepsProducingDistinctFramesAcrossLoads) {
    const QFileInfo fixture = wmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists()) {
        GTEST_SKIP() << "local WMF fixture is not present";
    }

    WindowsMediaEngine engine;
    QString error;
    QObject::connect(&engine, &MediaEngine::errorOccurred, [&error](const QString &message) {
        error = message;
    });
    auto *surface = dynamic_cast<WindowsMediaSurface *>(engine.videoSurface());
    ASSERT_NE(surface, nullptr);

    MediaSource source;
    source.path = fixture.absoluteFilePath();
    engine.load(source, MediaKind::Video);
    std::set<quint64> firstLoadSignatures;
    waitForMovingVideo(engine, surface, &firstLoadSignatures, &error);

    engine.stop();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    error.clear();
    engine.load(source, MediaKind::Video);
    std::set<quint64> secondLoadSignatures;
    waitForMovingVideo(engine, surface, &secondLoadSignatures, &error);
}

TEST(WindowsMediaEngine, EnvLocalVideoPathPreviewsThroughQuickView) {
    const QFileInfo fixture = configuredWmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists())
        GTEST_SKIP() << "configured WMF video and build fixture are both unavailable";
    const QString path = fixture.absoluteFilePath();

    Settings settings;
    QuickView view(settings);
    view.resize(640, 360);
    view.show();
    view.showFile(path);
    auto *surface = findWindowsMediaSurface(&view);
    ASSERT_NE(surface, nullptr);

    QString error;
    auto *info = view.findChild<QLabel *>(QStringLiteral("previewInfoLabel"));
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 7000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(25);
        if (info && info->text().contains(QStringLiteral("Media Foundation")))
            error = info->text();
    }

    EXPECT_TRUE(surface->isVisibleTo(&view));
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_FALSE(surface->currentFrameForTest().isNull()) << error.toStdString();
}

TEST(WindowsMediaEngine, EnvLocalVideoPathKeepsMovingThroughQuickView) {
    const QFileInfo fixture = configuredWmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists())
        GTEST_SKIP() << "configured WMF video and build fixture are both unavailable";
    const QString path = fixture.absoluteFilePath();

    Settings settings;
    QuickView view(settings);
    view.resize(640, 360);
    view.show();
    view.showFile(path);
    auto *surface = findWindowsMediaSurface(&view);
    ASSERT_NE(surface, nullptr);

    QString error;
    std::set<quint64> signatures;
    waitForQuickViewMovingVideo(view, surface, &signatures, &error);
}

TEST(WindowsMediaEngine, QuickViewKeepsVideoMovingAcrossSelections) {
    const QFileInfo fixture = wmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists()) {
        GTEST_SKIP() << "local WMF fixture is not present";
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("first.mp4"));
    const QString second = dir.filePath(QStringLiteral("second.mp4"));
    ASSERT_TRUE(QFile::copy(fixture.absoluteFilePath(), first));
    ASSERT_TRUE(QFile::copy(fixture.absoluteFilePath(), second));

    Settings settings;
    QuickView view(settings);
    view.resize(640, 360);
    view.show();

    view.showFile(first);
    auto *surface = findWindowsMediaSurface(&view);
    ASSERT_NE(surface, nullptr);
    QString error;
    std::set<quint64> firstSignatures;
    waitForQuickViewMovingVideo(view, surface, &firstSignatures, &error);

    error.clear();
    view.showFile(second);
    std::set<quint64> secondSignatures;
    waitForQuickViewMovingVideo(view, surface, &secondSignatures, &error);
}

// D:/usbhdd/123.avi produced "…event=5 param1=0x4 param2=0xc004f011", which is
// true and useless. 0xC004F011 is a Software Licensing code: the MPEG-2
// decoder is installed and Windows refuses to run it (measured -- three
// separate instantiation paths all return it). "No decoder" would be the wrong
// story, so this case gets its own wording.
TEST(WindowsMediaEngineErrorText, ALicenceRefusalIsNotReportedAsAMissingDecoder) {
    const QString text = WindowsMediaEngine::errorText(4, 0xc004f011,
                                                       QStringLiteral("D:/usbhdd/123.avi"));
    EXPECT_TRUE(text.contains(QStringLiteral("licens"), Qt::CaseInsensitive))
        << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("no decoder"), Qt::CaseInsensitive))
        << text.toStdString();
    // The raw codes stay, at the end, for diagnosis.
    EXPECT_TRUE(text.contains(QStringLiteral("c004f011"))) << text.toStdString();
    EXPECT_GT(text.size(), 40) << text.toStdString();
}

// A genuinely absent decoder -- Cinepak, Indeo, MS Video 1 all measured as
// having no MFT at all -- still names the extension, since that is what the
// user recognises.
TEST(WindowsMediaEngineErrorText, AMissingDecoderNamesTheExtension) {
    const QString text = WindowsMediaEngine::errorText(4, 0xc00d5212,
                                                       QStringLiteral("D:/clip.avi"));
    EXPECT_TRUE(text.contains(QStringLiteral(".avi"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("decoder"), Qt::CaseInsensitive))
        << text.toStdString();
}

// Codec packs are DirectShow filters and register nothing with Media
// Foundation, so telling the user to install one sends them to do something
// that cannot work. It was in the first version of this message.
TEST(WindowsMediaEngineErrorText, NoMessageSuggestsACodecPack) {
    for (unsigned long extended : {0xc004f011ul, 0xc00d5212ul, 0ul}) {
        const QString text = WindowsMediaEngine::errorText(4, extended,
                                                           QStringLiteral("D:/clip.avi"));
        EXPECT_FALSE(text.contains(QStringLiteral("codec pack"), Qt::CaseInsensitive))
            << text.toStdString();
    }
}

TEST(WindowsMediaEngineErrorText, DistinguishesTheOtherCodes) {
    const QString path = QStringLiteral("D:/clip.mp4");
    const QString decode = WindowsMediaEngine::errorText(3, 0, path);
    const QString unsupported = WindowsMediaEngine::errorText(4, 0, path);
    const QString encrypted = WindowsMediaEngine::errorText(5, 0, path);
    EXPECT_NE(decode, unsupported);
    EXPECT_NE(unsupported, encrypted);
    EXPECT_TRUE(decode.contains(QStringLiteral("damaged"), Qt::CaseInsensitive))
        << decode.toStdString();
    EXPECT_TRUE(encrypted.contains(QStringLiteral("protected"), Qt::CaseInsensitive))
        << encrypted.toStdString();
}

TEST(WindowsMediaEngineErrorText, AnExtensionlessPathStillGetsASentence) {
    const QString text = WindowsMediaEngine::errorText(4, 0, QStringLiteral("D:/usbhdd/clip"));
    EXPECT_FALSE(text.isEmpty());
    EXPECT_TRUE(text.contains(QStringLiteral("decoder"), Qt::CaseInsensitive))
        << text.toStdString();
}

// The seek wedge, end to end. Needs a file Media Foundation cannot seek --
// point FILECOMMANDER_WMF_UNSEEKABLE_VIDEO at one to run it. The engine must
// come back to life on its own and say what happened, rather than sitting on a
// frozen frame forever (measured: 60 s with no error and no recovery before
// this change).
//
// Which of the two signals it uses depends on WHY the seek failed, and the
// file this was measured on is an unfinished download, so it reports the file
// rather than the seek. A wedge with the data present -- the same symptom, a
// different cause -- reports seekUnsupported instead; no file is known that
// produces it, so the test accepts either and checks the recovery, which is
// the same in both cases.
TEST(WindowsMediaEngine, AStuckSeekRecoversAndReportsItself) {
    const QString path =
        QString::fromLocal8Bit(qgetenv("FILECOMMANDER_WMF_UNSEEKABLE_VIDEO"));
    if (path.isEmpty() || !QFileInfo::exists(path))
        GTEST_SKIP() << "no known unseekable video configured";

    WindowsMediaEngine engine;
    QSignalSpy unsupported(&engine, &MediaEngine::seekUnsupported);
    QSignalSpy incomplete(&engine, &MediaEngine::mediaIncomplete);
    MediaSource source;
    source.path = path;
    engine.load(source, MediaKind::Video);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000 && engine.positionSeconds() < 2.0)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_GT(engine.durationSeconds(), 600.0) << "expected a long clip";
    ASSERT_GT(engine.positionSeconds(), 1.0) << "clip never started playing";

    engine.seekFraction(0.5);
    timer.restart();
    while (timer.elapsed() < 15000 && unsupported.isEmpty() && incomplete.isEmpty())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(unsupported.count() + incomplete.count(), 1)
        << "the wedged seek went unnoticed; position is " << engine.positionSeconds() << " of "
        << engine.durationSeconds();

    // Alive again: the reload has to actually produce playback, not just a
    // fresh engine sitting at zero.
    // The reload starts the clip again from the beginning, so "recovered" means
    // the clock is running THERE. Waiting for it to pass the position it was
    // stuck at would wait forever: the position goes down, not up. It also
    // takes a moment to get there -- until the new source is ready the engine
    // still reports the old time -- so the baseline is taken once the clock
    // reads somewhere near the start.
    timer.restart();
    double baseline = -1.0;
    bool moving = false;
    while (timer.elapsed() < 20000 && !moving) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const double now = engine.positionSeconds();
        if (now > 60.0)
            continue; // still reporting the wedged position
        if (baseline < 0.0)
            baseline = now;
        moving = now > baseline + 0.5;
    }
    EXPECT_TRUE(moving) << "playback did not resume after the reload";
    engine.stop();
}

// A seek that works must not be reported as a failure. The build fixture is
// two seconds long, so this also covers a target close to the end.
TEST(WindowsMediaEngine, AWorkingSeekIsNotReportedAsUnsupported) {
    const QFileInfo fixture = wmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists())
        GTEST_SKIP() << "local WMF fixture is not present";

    WindowsMediaEngine engine;
    QSignalSpy unsupported(&engine, &MediaEngine::seekUnsupported);
    MediaSource source;
    source.path = fixture.absoluteFilePath();
    engine.load(source, MediaKind::Video);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8000 && engine.durationSeconds() <= 0.0)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_GT(engine.durationSeconds(), 0.0);

    engine.seekFraction(0.5);
    timer.restart();
    // Well past the watchdog's deadline.
    while (timer.elapsed() < SeekWatchdog::kTimeoutMs + 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(unsupported.isEmpty()) << "a healthy seek was reported as wedged";
    engine.stop();
}

// An unfinished download plays until it reaches the part that was never
// written, and then Media Foundation keeps the clock running at full speed
// with a frozen picture: no error, no end-of-stream, state still Playing.
// Measured on one such file -- frames stopped at 134 s and the position was
// still climbing past 199 s a minute later. So the pane has to notice the
// PICTURE stopping, not the position.
//
// Point FILECOMMANDER_WMF_UNSEEKABLE_VIDEO at such a file to run this.
TEST(WindowsMediaEngine, AFrozenPictureOverAHoleIsReportedAsAnIncompleteFile) {
    const QString path =
        QString::fromLocal8Bit(qgetenv("FILECOMMANDER_WMF_UNSEEKABLE_VIDEO"));
    if (path.isEmpty() || !QFileInfo::exists(path))
        GTEST_SKIP() << "no known incomplete video configured";

    WindowsMediaEngine engine;
    QSignalSpy incomplete(&engine, &MediaEngine::mediaIncomplete);
    MediaSource source;
    source.path = path;
    engine.load(source, MediaKind::Video);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000 && engine.durationSeconds() <= 0.0)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_GT(engine.durationSeconds(), 600.0) << "expected a long clip";

    // Start inside the written part and let playback walk off the end of it.
    // 110 s is inside the data on the measured file; a healthy seek there is
    // itself part of what this checks.
    engine.seekFraction(110.0 / engine.durationSeconds());
    timer.restart();
    while (timer.elapsed() < 60000 && incomplete.isEmpty())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(incomplete.count(), 1)
        << "playback ran into the hole unreported; position is " << engine.positionSeconds();

    // Said once, and the clock is stopped -- otherwise it runs on to the end of
    // a film that is not there.
    const double stoppedAt = engine.positionSeconds();
    timer.restart();
    while (timer.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(incomplete.count(), 1) << "reported more than once";
    EXPECT_LT(engine.positionSeconds() - stoppedAt, 1.0) << "the clock kept running";
    engine.stop();
}

// A complete file must never be accused, however long it plays.
TEST(WindowsMediaEngine, ACompleteFileIsNeverReportedAsIncomplete) {
    const QFileInfo fixture = wmfFixture(QStringLiteral("video-h264.mp4"));
    if (!fixture.exists())
        GTEST_SKIP() << "local WMF fixture is not present";

    WindowsMediaEngine engine;
    QSignalSpy incomplete(&engine, &MediaEngine::mediaIncomplete);
    MediaSource source;
    source.path = fixture.absoluteFilePath();
    engine.load(source, MediaKind::Video);

    QElapsedTimer timer;
    timer.start();
    // Long enough to play the two-second fixture out several times over.
    while (timer.elapsed() < 12000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(incomplete.isEmpty()) << "a complete file was called incomplete";
    engine.stop();
}
