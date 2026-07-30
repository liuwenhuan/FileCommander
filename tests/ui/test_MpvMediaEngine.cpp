#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWidget>

#include <clocale>
#include <memory>

#include <mpv/client.h>

#include "QuickView.h"
#include "config/Settings.h"
#include "media/MpvMediaEngine.h"

namespace {

QString shellQuote(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString wslPath(QString path) {
    path = QDir::fromNativeSeparators(QDir::cleanPath(path));
    if (path.size() > 2 && path.at(1) == QLatin1Char(':')) {
        const QChar drive = path.at(0).toLower();
        return QStringLiteral("/mnt/%1/%2").arg(drive, path.mid(3));
    }
    return path;
}

bool runFfmpeg(const QStringList &arguments, const QString &outputPath) {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!ffmpeg.isEmpty())
        return QProcess::execute(ffmpeg, arguments + QStringList{outputPath}) == 0;

#ifdef Q_OS_WIN
    const QString wsl = QStandardPaths::findExecutable(QStringLiteral("wsl.exe"));
    if (wsl.isEmpty())
        return false;
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (const QString &argument : arguments)
        quoted.append(shellQuote(argument));
    const QString command =
        QStringLiteral("ffmpeg %1 %2")
            .arg(quoted.join(QLatin1Char(' ')), shellQuote(wslPath(outputPath)));
    return QProcess::execute(wsl, {QStringLiteral("sh"), QStringLiteral("-lc"), command}) == 0;
#else
    Q_UNUSED(outputPath);
    return false;
#endif
}

class MpvMediaEngine : public testing::Test {
protected:
    static void SetUpTestSuite() {
        std::setlocale(LC_NUMERIC, "C");
        fixtureDir = std::make_unique<QTemporaryDir>();
        if (!fixtureDir->isValid())
            return;

        audioPath = fixtureDir->filePath(QStringLiteral("media-audio.wav"));
        videoPath = fixtureDir->filePath(QStringLiteral("media-video.mp4"));
        secondVideoPath = fixtureDir->filePath(QStringLiteral("media-video-2.mp4"));

        const bool audioReady = runFfmpeg(
            {QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
             QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
             QStringLiteral("sine=frequency=440:duration=1"), QStringLiteral("-metadata"),
             QStringLiteral("title=Fixture title"), QStringLiteral("-c:a"),
             QStringLiteral("pcm_s16le")},
            audioPath);
        const bool videoReady = runFfmpeg(
            {QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
             QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
             QStringLiteral("testsrc=size=320x180:rate=24:duration=1"),
             QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-c:v"),
             QStringLiteral("libx264")},
            videoPath);
        if (!audioReady || !videoReady || !QFile::copy(videoPath, secondVideoPath)) {
            audioPath.clear();
            videoPath.clear();
            secondVideoPath.clear();
        }
    }

    static void TearDownTestSuite() {
        fixtureDir.reset();
    }

    bool fixturesReady() const { return !audioPath.isEmpty() && !videoPath.isEmpty(); }

    static ::MpvMediaEngine::Options headlessOptions() {
        ::MpvMediaEngine::Options options;
        options.headless = true;
        return options;
    }

    static bool waitForState(::MpvMediaEngine &engine, MediaState wanted,
                             int timeoutMs = 5000) {
        if (engine.state() == wanted)
            return true;
        QSignalSpy states(&engine, &MediaEngine::stateChanged);
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            if (!states.wait(100) && engine.state() != wanted)
                continue;
            if (engine.state() == wanted)
                return true;
        }
        return false;
    }

    static bool waitForMetadata(::MpvMediaEngine &engine, int timeoutMs = 3000) {
        if (!engine.metadata().isEmpty())
            return true;
        QSignalSpy metadataChanges(&engine, &MediaEngine::metadataChanged);
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            metadataChanges.wait(100);
            if (!engine.metadata().isEmpty())
                return true;
        }
        return false;
    }

    static std::unique_ptr<QTemporaryDir> fixtureDir;
    static QString audioPath;
    static QString videoPath;
    static QString secondVideoPath;
};

std::unique_ptr<QTemporaryDir> MpvMediaEngine::fixtureDir;
QString MpvMediaEngine::audioPath;
QString MpvMediaEngine::videoPath;
QString MpvMediaEngine::secondVideoPath;

TEST_F(MpvMediaEngine, QuickViewUsesOneContextAcrossAudioVideoAudio) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    int creates = 0;
    int initializes = 0;
    ::MpvMediaEngine::Options options = headlessOptions();
    options.createContext = [&creates]() {
        ++creates;
        return mpv_create();
    };
    options.initializeContext = [&initializes](mpv_handle *handle) {
        ++initializes;
        return mpv_initialize(handle);
    };

    auto engine = std::make_unique<::MpvMediaEngine>(std::move(options));
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    Settings settings(settingsDir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));

    view.showFile(audioPath);
    view.showFile(videoPath);
    view.showFile(audioPath);

    EXPECT_EQ(creates, 1);
    EXPECT_EQ(initializes, 1);
    EXPECT_EQ(view.findChildren<::MpvMediaEngine *>().size(), 1);
}

TEST_F(MpvMediaEngine, QuickViewReselectDoesNotStopTheCurrentAudioOrVideo) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    auto engine = std::make_unique<::MpvMediaEngine>(headlessOptions());
    auto *sharedEngine = engine.get();
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    Settings settings(settingsDir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded, nullptr, std::move(engine));

    view.showFile(audioPath);
    ASSERT_TRUE(waitForState(*sharedEngine, MediaState::Playing));
    view.showFile(audioPath);
    EXPECT_NE(sharedEngine->state(), MediaState::Idle);
    EXPECT_EQ(sharedEngine->currentSource().path, audioPath);

    view.showFile(videoPath);
    ASSERT_TRUE(waitForState(*sharedEngine, MediaState::Playing));
    view.showFile(videoPath);
    EXPECT_NE(sharedEngine->state(), MediaState::Idle);
    EXPECT_EQ(sharedEngine->currentSource().path, videoPath);
}

TEST_F(MpvMediaEngine, AudioVideoAudioTransitionsReuseTheCurrentSourceSlot) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();

    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_EQ(engine.currentKind(), MediaKind::Audio);
    EXPECT_EQ(engine.videoMode(), QStringLiteral("no"));

    engine.load({videoPath, {}, false}, MediaKind::Video);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_EQ(engine.currentKind(), MediaKind::Video);
    EXPECT_EQ(engine.videoMode(), QStringLiteral("auto"));

    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_EQ(engine.currentSource().path, audioPath);
    EXPECT_EQ(engine.currentKind(), MediaKind::Audio);
}

TEST_F(MpvMediaEngine, VideoSpeedDoesNotCarryIntoAudioAndIsRememberedForVideo) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    engine.setSpeed(2.0);

    engine.load({videoPath, {}, false}, MediaKind::Video);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_DOUBLE_EQ(engine.playbackSpeed(), 2.0);

    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_DOUBLE_EQ(engine.playbackSpeed(), 1.0);

    engine.load({secondVideoPath, {}, false}, MediaKind::Video);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_DOUBLE_EQ(engine.playbackSpeed(), 2.0);
}

TEST_F(MpvMediaEngine, VideoToVideoKeepsOneStableSurface) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    QWidget *surface = engine.videoSurface();

    engine.load({videoPath, {}, false}, MediaKind::Video);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    engine.load({secondVideoPath, {}, false}, MediaKind::Video);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));

    EXPECT_EQ(engine.currentSource().path, secondVideoPath);
    EXPECT_EQ(engine.videoSurface(), surface);
}

TEST_F(MpvMediaEngine, RapidAlternatingLoadsKeepOnlyTheLastSource) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();

    for (int i = 0; i < 8; ++i) {
        const bool audio = (i % 2) == 0;
        engine.load({audio ? audioPath : videoPath, {}, false},
                    audio ? MediaKind::Audio : MediaKind::Video);
    }
    engine.load({secondVideoPath, {}, false}, MediaKind::Video);

    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_EQ(engine.currentSource().path, secondVideoPath);
    EXPECT_EQ(engine.currentKind(), MediaKind::Video);
}

TEST_F(MpvMediaEngine, VolumeMuteAndSeekApplyToTheSharedCore) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    engine.setVolume(37);
    engine.setMute(true);
    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));

    EXPECT_EQ(engine.volume(), 37);
    EXPECT_TRUE(engine.muted());
    engine.playPause();
    ASSERT_TRUE(waitForState(engine, MediaState::Paused));
    engine.seekFraction(0.5);
    EXPECT_NEAR(engine.positionSeconds(), 0.5, 0.25);
}

TEST_F(MpvMediaEngine, EndedPlayPauseReplaysTheSameSource) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    engine.load({audioPath, {}, false}, MediaKind::Audio);

    ASSERT_TRUE(waitForState(engine, MediaState::Ended, 8000));
    const QString sourceBeforeReplay = engine.currentSource().path;
    engine.playPause();

    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_EQ(engine.currentSource().path, sourceBeforeReplay);
}

TEST_F(MpvMediaEngine, EndedSourceCanBeReplacedByANewLoad) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Ended, 8000));

    engine.load({videoPath, {}, false}, MediaKind::Video);

    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    EXPECT_EQ(engine.currentSource().path, videoPath);
    EXPECT_EQ(engine.currentKind(), MediaKind::Video);
}

TEST_F(MpvMediaEngine, ReplacementClearsPriorMetadataBeforeTheNewFileLoads) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    ASSERT_TRUE(waitForMetadata(engine));

    engine.load({videoPath, {}, false}, MediaKind::Video);

    EXPECT_TRUE(engine.metadata().isEmpty());
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
}

TEST_F(MpvMediaEngine, StopReturnsIdleAndClearsMetadata) {
    if (!fixturesReady())
        GTEST_SKIP() << "ffmpeg could not generate the local media fixtures";
    ::MpvMediaEngine engine(headlessOptions());
    engine.initialize();
    engine.load({audioPath, {}, false}, MediaKind::Audio);
    ASSERT_TRUE(waitForState(engine, MediaState::Playing));
    ASSERT_TRUE(waitForMetadata(engine));

    engine.stop();

    EXPECT_EQ(engine.state(), MediaState::Idle);
    EXPECT_TRUE(engine.metadata().isEmpty());
    EXPECT_TRUE(engine.currentSource().path.isEmpty());
}

} // namespace
