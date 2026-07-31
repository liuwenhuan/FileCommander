#pragma once

#include "MediaTypes.h"

#include <QImage>
#include <QWidget>

class WindowsMediaSurface final : public QWidget {
public:
    explicit WindowsMediaSurface(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void setVideoEffect(const VideoEffectSettings &settings);

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
};
