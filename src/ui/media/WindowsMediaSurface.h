#pragma once

#include "MediaTypes.h"

#include <QImage>
#include <QWidget>

class WindowsMediaSurface final : public QWidget {
public:
    explicit WindowsMediaSurface(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void setVideoEffect(const VideoEffectSettings &settings);

    // Quarter turns only, normalised to 0/90/180/270. Applied when the frame is
    // painted rather than to the frame itself: a rotation is a way of LOOKING at
    // the clip, and rotating every decoded frame would cost a full-resolution
    // transform per frame for something the painter does on the way to screen
    // anyway.
    void setRotation(int degrees);
    int rotation() const { return m_rotation; }

    static QImage applyVideoEffectForTest(const QImage &frame,
                                          const VideoEffectSettings &settings);
    QImage currentFrameForTest() const { return m_frame; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static QImage applyVideoEffect(const QImage &frame, const VideoEffectSettings &settings);

    QImage m_sourceFrame;
    QImage m_frame;
    VideoEffectSettings m_effect;
    int m_rotation = 0;
};
