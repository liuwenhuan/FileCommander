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
#include <memory>
#include <set>
#include <utility>

#include "FileProvider.h"
#include "QuickView.h"
#include "config/Settings.h"
#include "media/MediaEngine.h"
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
