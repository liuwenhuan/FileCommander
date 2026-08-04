#pragma once

#include "MediaTypes.h"

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QSize>

class QWidget;

Q_DECLARE_METATYPE(MediaKind)
Q_DECLARE_METATYPE(MediaState)

class MediaEngine : public QObject {
    Q_OBJECT

public:
    explicit MediaEngine(QObject *parent = nullptr) : QObject(parent) {}
    ~MediaEngine() override = default;

    virtual void initialize() = 0;
    virtual void load(const MediaSource &source, MediaKind kind) = 0;
    virtual void stop() {}
    virtual void playPause() {}
    virtual void seekFraction(double) {}
    virtual void setVolume(int) {}
    virtual void setMute(bool) {}
    virtual void setSpeed(double) {}
    virtual void setVideoEffect(const VideoEffectSettings &) {}
    // Quarter turns, 0/90/180/270 clockwise. A no-op by default like the rest
    // of the optional capabilities above, so a backend that cannot rotate (and
    // every stub in the tests) needs no change.
    virtual void setVideoRotation(int) {}
    virtual QWidget *videoSurface() = 0;

    virtual MediaState state() const { return MediaState::Idle; }
    virtual MediaKind currentKind() const { return MediaKind::Audio; }
    virtual MediaSource currentSource() const { return {}; }
    virtual double durationSeconds() const { return 0.0; }
    virtual double positionSeconds() const { return 0.0; }
    virtual bool paused() const { return state() == MediaState::Paused; }
    virtual bool ended() const { return state() == MediaState::Ended; }
    virtual int volume() const { return 0; }
    virtual bool muted() const { return false; }
    virtual QHash<QString, QString> metadata() const { return {}; }
    virtual QString metadataValue(const QString &key) const { return metadata().value(key); }
    virtual QSize currentVideoSize() const { return {}; }
    virtual QString videoCodec() const { return {}; }
    virtual QString videoMode() const { return {}; }

    // A page the user can go to in order to fix the last failure themselves --
    // currently only the missing-decoder case, where installing one is the
    // whole remedy and the app can do nothing else. Empty when there is
    // nothing useful to point at, which is the normal case.
    virtual QString lastErrorHelpUrl() const { return {}; }

    // The backend's reported duration is provably wrong, so there is no total
    // length to show and no fraction to seek by.
    //
    // MPEG program streams carry no duration field at all, and Media Foundation
    // has to guess. Measured on a 438 MB, 8.5-minute file whose SCR wraps its
    // 33 bits in the first packet: it guesses 7.47 s, never revises it, plays
    // happily past it -- and CLAMPS every seek to it, so a request for 480 s
    // returns S_OK and lands at 7.47. Knowing the real length would not help;
    // the engine will not go there.
    virtual bool durationIsUnknown() const { return false; }

signals:
    void stateChanged(MediaState state);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void metadataChanged(const QHash<QString, QString> &metadata);
    void videoSizeChanged(const QSize &size);
    void errorOccurred(const QString &message);

    // A seek was accepted and then never finished, so the backend had to
    // reload the clip to get it playing again -- see SeekWatchdog. Distinct
    // from errorOccurred because playback survives: the pane says so and keeps
    // showing the video rather than replacing it with a failure page.
    void seekUnsupported();

    // The file has nothing but zeros where playback needs data -- an
    // unfinished download keeps its full size, so this is indistinguishable
    // from a complete file until something tries to read that far. Reported
    // once per clip; the pane says so and stops pretending to play.
    void mediaIncomplete();
};
