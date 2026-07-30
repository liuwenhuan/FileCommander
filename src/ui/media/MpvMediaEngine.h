#pragma once

#include "MediaEngine.h"

#include <QHash>
#include <QPointer>
#include <QSize>

#include <functional>
#include <memory>

struct mpv_handle;
class MpvVideoSurface;
class QTimer;

class MpvMediaEngine final : public MediaEngine {
    Q_OBJECT

public:
    struct Options {
        bool headless = false;
        std::function<mpv_handle *()> createContext;
        std::function<int(mpv_handle *)> initializeContext;
    };

    explicit MpvMediaEngine(QObject *parent = nullptr);
    explicit MpvMediaEngine(Options options, QObject *parent = nullptr);
    ~MpvMediaEngine() override;

    void initialize() override;
    void load(const MediaSource &source, MediaKind kind) override;
    void stop() override;
    void playPause() override;
    void seekFraction(double fraction) override;
    void setVolume(int volume) override;
    void setMute(bool mute) override;
    void setSpeed(double speed) override;
    void setVideoFilter(const QString &filter) override;
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
    QHash<QString, QString> metadata() const override;
    QString metadataValue(const QString &key) const override;
    QSize currentVideoSize() const override;
    QString videoCodec() const override;
    QString videoMode() const override;

private slots:
    void processEvents();
    void pollProperties();

private:
    friend class MpvVideoSurface;

    static void onWakeup(void *context);
    mpv_handle *handle() const;
    void videoSurfaceReady();
    void issueLoad();
    void setState(MediaState state);
    void clearObservedValues();
    void refreshMetadata();
    double getDouble(const char *property) const;
    long long getInt(const char *property) const;
    bool getFlag(const char *property) const;
    QString getString(const char *property) const;

    Options m_options;
    mpv_handle *m_mpv = nullptr;
    QPointer<MpvVideoSurface> m_surface;
    QTimer *m_pollTimer = nullptr;
    MediaSource m_source;
    MediaKind m_kind = MediaKind::Audio;
    MediaState m_state = MediaState::Idle;
    QHash<QString, QString> m_metadata;
    QSize m_videoSize;
    QString m_videoCodec;
    QString m_videoMode;
    QString m_videoFilter;
    double m_duration = 0.0;
    double m_position = 0.0;
    double m_speed = 1.0;
    int m_volume = 100;
    bool m_muted = false;
    bool m_initialized = false;
    bool m_surfaceReady = false;
    bool m_pendingVideoLoad = false;
};
