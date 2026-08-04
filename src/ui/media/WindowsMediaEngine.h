#pragma once

#include "MediaEngine.h"
#include "SeekWatchdog.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QSize>

#include <memory>

class QTimer;
class WindowsMediaSurface;

struct IMFMediaEngine;
struct IMFMediaEngineNotify;
struct IWICImagingFactory;

class WindowsMediaEngine final : public MediaEngine {
    Q_OBJECT

public:
    explicit WindowsMediaEngine(QObject *parent = nullptr);
    ~WindowsMediaEngine() override;

    static QString sourceUrlForMediaFoundation(const MediaSource &source);

    // Turns an MF_MEDIA_ENGINE_ERR_* code (the ERROR event's param1) plus its
    // extended HRESULT into something a person can act on. Public so the
    // wording can be tested without a media file that provokes the error.
    static QString errorText(quint64 code, unsigned long extended, const QString &path);

    void initialize() override;
    void load(const MediaSource &source, MediaKind kind) override;
    void stop() override;
    void playPause() override;
    void seekFraction(double fraction) override;
    void setVolume(int volume) override;
    void setMute(bool mute) override;
    void setSpeed(double speed) override;
    void setVideoEffect(const VideoEffectSettings &settings) override;
    void setVideoRotation(int degrees) override;
    QWidget *videoSurface() override;

    MediaState state() const override;
    MediaKind currentKind() const override;
    MediaSource currentSource() const override;
    double durationSeconds() const override;
    double positionSeconds() const override;
    bool paused() const override;
    bool ended() const override;
    int volume() const override;
    bool muted() const override;
    QSize currentVideoSize() const override;
    QString videoCodec() const override;

private:
    class Notify;

    void setState(MediaState state);
    void setFailure(const QString &message);
    void clearObservedValues();
    void updateTimeline();
    void recoverFromStuckSeek();
    void updateVideoSize();
    void pumpFrame();
    void onMediaEvent(unsigned long event, quint64 param1 = 0, unsigned long param2 = 0);

    std::unique_ptr<Notify> m_notify;
    IMFMediaEngine *m_engine = nullptr;
    IWICImagingFactory *m_wic = nullptr;
    QPointer<WindowsMediaSurface> m_surface;
    int m_rotation = 0;
    QTimer *m_pollTimer = nullptr;
    MediaSource m_source;
    MediaKind m_kind = MediaKind::Audio;
    MediaState m_state = MediaState::Idle;
    QSize m_videoSize;
    QString m_videoCodec;
    double m_duration = 0.0;
    double m_position = 0.0;
    double m_speed = 1.0;
    int m_volume = 100;
    bool m_muted = false;
    bool m_initialized = false;
    bool m_comInitialized = false;
    bool m_sawVideoFrame = false;
    SeekWatchdog m_seekWatchdog;
    QElapsedTimer m_clock; // monotonic, feeds the watchdog
    quint64 m_loadGeneration = 0;
};
