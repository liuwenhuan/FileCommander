#include <gtest/gtest.h>

#include <QHash>
#include <QPointer>
#include <QSignalSpy>
#include <QWidget>

#include <memory>

#include "media/MediaEngine.h"
#include "media/NullMediaEngine.h"

namespace {

class FakeMediaEngine final : public MediaEngine {
public:
    explicit FakeMediaEngine(QObject *parent = nullptr) : MediaEngine(parent) {}

    void initialize() override { m_initialized = true; }

    void load(const MediaSource &source, MediaKind kind) override {
        m_source = source;
        m_kind = kind;
        m_hasSource = true;
        m_volumeAtLastLoad = m_volume;
        m_mutedAtLastLoad = m_muted;
        m_metadata = {{QStringLiteral("title"), source.path}};
        setState(MediaState::Playing);
        emit metadataChanged(m_metadata);
    }

    void stop() override {
        m_hasSource = false;
        m_source = {};
        m_metadata.clear();
        setState(MediaState::Idle);
        emit metadataChanged(m_metadata);
    }

    void playPause() override {
        if (!m_hasSource)
            return;
        if (m_state == MediaState::Ended) {
            ++m_restartCount;
            setState(MediaState::Playing);
            return;
        }
        setState(m_state == MediaState::Playing ? MediaState::Paused : MediaState::Playing);
    }

    void seekFraction(double fraction) override { m_lastSeekFraction = fraction; }
    void setVolume(int volume) override { m_volume = volume; }
    void setMute(bool mute) override { m_muted = mute; }
    QWidget *videoSurface() override {
        ++m_videoSurfaceRequests;
        if (!m_videoSurface)
            m_videoSurface = std::make_unique<QWidget>();
        return m_videoSurface.get();
    }

    void finish() { setState(MediaState::Ended); }

    bool initialized() const { return m_initialized; }
    bool hasSource() const { return m_hasSource; }
    const MediaSource &source() const { return m_source; }
    MediaKind kind() const { return m_kind; }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    int volumeAtLastLoad() const { return m_volumeAtLastLoad; }
    bool mutedAtLastLoad() const { return m_mutedAtLastLoad; }
    int restartCount() const { return m_restartCount; }
    double lastSeekFraction() const { return m_lastSeekFraction; }
    const QHash<QString, QString> &metadata() const { return m_metadata; }
    MediaState state() const { return m_state; }
    int videoSurfaceRequests() const { return m_videoSurfaceRequests; }

private:
    void setState(MediaState state) {
        if (m_state == state)
            return;
        m_state = state;
        emit stateChanged(m_state);
    }

    bool m_initialized = false;
    bool m_hasSource = false;
    MediaSource m_source;
    MediaKind m_kind = MediaKind::Audio;
    MediaState m_state = MediaState::Idle;
    std::unique_ptr<QWidget> m_videoSurface;
    int m_volume = 100;
    bool m_muted = false;
    int m_volumeAtLastLoad = 100;
    bool m_mutedAtLastLoad = false;
    int m_restartCount = 0;
    double m_lastSeekFraction = 0.0;
    QHash<QString, QString> m_metadata;
    int m_videoSurfaceRequests = 0;
};

class MediaPresenter {
public:
    explicit MediaPresenter(MediaEngine &engine) : m_engine(engine) {}

    void prepare(int volume, bool muted) {
        m_engine.setVolume(volume);
        m_engine.setMute(muted);
        m_engine.initialize();
    }

    void show(const QString &path, MediaKind kind) {
        m_engine.load({path, {}, false}, kind);
        m_visibleVideoSurface = kind == MediaKind::Video ? m_engine.videoSurface() : nullptr;
    }

    QWidget *visibleVideoSurface() const { return m_visibleVideoSurface; }

private:
    MediaEngine &m_engine;
    QPointer<QWidget> m_visibleVideoSurface;
};

} // namespace

TEST(MediaEngineContract, LoadReplacesThePriorSource) {
    FakeMediaEngine engine;
    MediaPresenter presenter(engine);

    presenter.show(QStringLiteral("first.mp3"), MediaKind::Audio);
    presenter.show(QStringLiteral("second.mp4"), MediaKind::Video);

    EXPECT_TRUE(engine.hasSource());
    EXPECT_EQ(engine.source().path, QStringLiteral("second.mp4"));
    EXPECT_EQ(engine.kind(), MediaKind::Video);
}

TEST(MediaEngineContract, AudioDoesNotRequireAVisibleVideoSurface) {
    FakeMediaEngine engine;
    MediaPresenter presenter(engine);

    presenter.show(QStringLiteral("song.flac"), MediaKind::Audio);

    EXPECT_EQ(presenter.visibleVideoSurface(), nullptr);
    EXPECT_TRUE(engine.hasSource());
    EXPECT_EQ(engine.videoSurfaceRequests(), 0);
}

TEST(MediaEngineContract, VideoExposesOneStableSurface) {
    FakeMediaEngine engine;
    MediaPresenter presenter(engine);

    presenter.show(QStringLiteral("first.mp4"), MediaKind::Video);
    QWidget *firstSurface = presenter.visibleVideoSurface();
    presenter.show(QStringLiteral("second.mp4"), MediaKind::Video);

    ASSERT_NE(firstSurface, nullptr);
    EXPECT_EQ(presenter.visibleVideoSurface(), firstSurface);
    EXPECT_EQ(engine.videoSurface(), firstSurface);
}

TEST(MediaEngineContract, VolumeAndMuteAreAppliedBeforeLoad) {
    FakeMediaEngine engine;
    MediaPresenter presenter(engine);

    presenter.prepare(37, true);
    presenter.show(QStringLiteral("song.ogg"), MediaKind::Audio);

    EXPECT_TRUE(engine.initialized());
    EXPECT_EQ(engine.volumeAtLastLoad(), 37);
    EXPECT_TRUE(engine.mutedAtLastLoad());
}

TEST(MediaEngineContract, PlayPauseRestartsAnEndedSource) {
    FakeMediaEngine engine;
    MediaPresenter presenter(engine);
    presenter.show(QStringLiteral("clip.webm"), MediaKind::Video);
    engine.finish();

    engine.playPause();

    EXPECT_EQ(engine.restartCount(), 1);
}

TEST(MediaEngineContract, StopReturnsToIdleAndClearsMetadata) {
    FakeMediaEngine engine;
    MediaPresenter presenter(engine);
    presenter.show(QStringLiteral("song.mp3"), MediaKind::Audio);
    ASSERT_FALSE(engine.metadata().isEmpty());

    engine.stop();

    EXPECT_FALSE(engine.hasSource());
    EXPECT_TRUE(engine.metadata().isEmpty());
    EXPECT_EQ(engine.state(), MediaState::Idle);
}

TEST(MediaEngineContract, NullEngineReportsUnsupportedLoadWithoutPlayback) {
    NullMediaEngine engine;
    QString error;
    QSignalSpy stateChanges(&engine, &MediaEngine::stateChanged);
    QObject::connect(&engine, &MediaEngine::errorOccurred, [&error](const QString &message) {
        error = message;
    });

    engine.initialize();
    engine.setVolume(25);
    engine.setMute(true);
    engine.load({QStringLiteral("clip.mp4"), {}, false}, MediaKind::Video);

    EXPECT_FALSE(error.isEmpty());
    EXPECT_NE(error.indexOf(QStringLiteral("not available"), 0), -1);
    ASSERT_EQ(stateChanges.count(), 1);
    EXPECT_EQ(qvariant_cast<MediaState>(stateChanges.at(0).at(0)), MediaState::Failed);
    EXPECT_EQ(engine.videoSurface(), nullptr);
}
