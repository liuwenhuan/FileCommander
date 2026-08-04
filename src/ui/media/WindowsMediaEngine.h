#pragma once

#include "MediaEngine.h"
#include "SeekWatchdog.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
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
    void checkForFrozenPicture();
    // Why the file is being sampled, which decides what is said when the
    // answer comes back.
    enum class HoleQuestion { None, AfterStuckSeek, AfterFrozenPicture };
    // Samples the file around `seconds` on a worker.
    //
    // Reading a hole is free -- the pages are never fetched, measured at 0-1 ms
    // per MiB against 86 ms for real data -- so the case this exists for is
    // cheap. The case it must not punish is the other one: a picture that froze
    // for some reason unrelated to the file, where all 64 samples land on real
    // bytes and each costs a seek. That is why it runs off the GUI thread.
    //
    // There is no test pinning this. The only file that reaches the code path
    // is one full of holes, where doing it synchronously is just as fast, so a
    // test would pass either way -- checked by putting the work back on this
    // thread and watching the test not care.
    void askWhetherFileIsIncomplete(double seconds, HoleQuestion question);
    void answerAboutIncompleteFile();
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
    double m_seekTarget = 0.0;
    QFutureWatcher<bool> *m_holeWatcher = nullptr;
    HoleQuestion m_holeQuestion = HoleQuestion::None;
    quint64 m_holeGeneration = 0;
    double m_positionAtLastFrame = 0.0; // clock reading when a picture last changed
    bool m_reportedIncomplete = false;
    QElapsedTimer m_clock; // monotonic, feeds the watchdog
    quint64 m_loadGeneration = 0;
};
