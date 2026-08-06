#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

class QMovie;

// Plays an animated image (GIF, animated WebP) and hands out finished frames.
//
// Separate from the still-image path because the two want opposite things. A
// still is decoded once on a worker thread and then transformed on demand --
// rotated, zoomed, re-tinted -- from the original kept in memory. An animation
// is decoded continuously, on the GUI thread (QMovie's timer lives there), and
// every frame is thrown away as the next arrives.
//
// Lives in `viewer` so both the preview pane and the F3 window drive animation
// the same way; they had drifted apart on stills already.
//
// Frames arrive already recoloured for the theme. Doing it here rather than in
// each caller is what keeps the two consistent, and it is where the cost can be
// measured in one place -- see frameCostUs().
class AnimatedImage : public QObject {
    Q_OBJECT

public:
    explicit AnimatedImage(QObject *parent = nullptr);
    ~AnimatedImage() override;

    // True when this file is an animation worth playing: the format supports
    // one AND the file actually carries more than a single frame. A one-frame
    // GIF is a still and is better served by the still path, which can rotate
    // and zoom it.
    static bool isAnimated(const QString &path);

    // Starts playing. Returns false if the file will not open or has no frames,
    // in which case the caller should fall back to the still path.
    bool play(const QString &path);
    void stop();

    void setPaused(bool paused);
    bool isPaused() const;
    bool isPlaying() const;

    QSize frameSize() const;
    int frameCount() const;

    // Average microseconds spent recolouring one frame, over the frames shown
    // so far. Zero when no theme tint is in force, since nothing is done then.
    qint64 frameCostUs() const;

signals:
    // A frame, already recoloured for the current theme.
    void frameReady(const QImage &frame);

private:
    void onFrameChanged();

    struct Private;
    std::unique_ptr<Private> d;
};
