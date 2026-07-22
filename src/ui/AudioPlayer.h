#pragma once

#include <QObject>
#include <QString>

struct mpv_handle;

// A minimal audio-only playback engine over libmpv, mirroring the control
// surface of MpvWidget but without any video output. Because there is no video
// track to display, no OpenGL/render context is needed: the core is created
// with video disabled and plays straight to the system audio device, so a
// loaded file starts immediately (no widget needs to be shown first).
//
// Position/duration are polled by the owner on a timer, exactly like the video
// page; the mpv event queue is drained on the GUI thread so end-of-file is
// reported authoritatively.
class AudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    void load(const QString &path);
    void stop();
    void playPause();
    void seekFraction(double fraction); // absolute seek to fraction*duration
    void setVolume(int volume);         // 0..100
    void setMute(bool mute);

    double durationSeconds() const;
    double positionSeconds() const;
    bool paused() const;
    bool ended() const; // playback finished at EOF

    // Basic tags straight from the core's demuxer metadata, used as a fallback
    // for formats the hand-rolled ID3 reader does not cover (ogg/flac/…).
    QString metadata(const QString &key) const; // e.g. "title", "artist", "album"

signals:
    void mpvEvents(); // queued from mpv's thread; drained on the GUI thread

private slots:
    void onMpvEvents();

private:
    double getDouble(const char *prop) const;
    bool getFlag(const char *prop) const;

    static void onWakeup(void *ctx);

    mpv_handle *m_mpv = nullptr;
    QString m_currentPath; // last file, so playPause() can replay after EOF
    bool m_ended = false;
};
