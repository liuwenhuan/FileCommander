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

signals:
    void stateChanged(MediaState state);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void metadataChanged(const QHash<QString, QString> &metadata);
    void videoSizeChanged(const QSize &size);
    void errorOccurred(const QString &message);
};
