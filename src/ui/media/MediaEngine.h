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
    virtual QWidget *videoSurface() = 0;

signals:
    void stateChanged(MediaState state);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void metadataChanged(const QHash<QString, QString> &metadata);
    void videoSizeChanged(const QSize &size);
    void errorOccurred(const QString &message);
};
