#include "WindowsMediaSurface.h"

#include "theme/Phosphor.h"

#include <QPainter>
#include <QRect>

WindowsMediaSurface::WindowsMediaSurface(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

void WindowsMediaSurface::setFrame(const QImage &frame) {
    m_sourceFrame = frame;
    m_frame = applyVideoEffect(m_sourceFrame, m_effect);
    update();
}

void WindowsMediaSurface::setVideoEffect(const VideoEffectSettings &settings) {
    if (m_effect.tint == settings.tint && m_effect.pixelBlock == settings.pixelBlock)
        return;
    m_effect = settings;
    if (!m_sourceFrame.isNull())
        m_frame = applyVideoEffect(m_sourceFrame, m_effect);
    update();
}

QImage WindowsMediaSurface::applyVideoEffectForTest(const QImage &frame,
                                                    const VideoEffectSettings &settings) {
    return applyVideoEffect(frame, settings);
}

void WindowsMediaSurface::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (m_frame.isNull())
        return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QSize targetSize = m_frame.size().scaled(rect().size(), Qt::KeepAspectRatio);
    const QPoint targetTopLeft((width() - targetSize.width()) / 2,
                               (height() - targetSize.height()) / 2);
    const QRect target(targetTopLeft, targetSize);
    painter.drawImage(target, m_frame);
}

QImage WindowsMediaSurface::applyVideoEffect(const QImage &frame,
                                             const VideoEffectSettings &settings) {
    if (frame.isNull())
        return frame;
    QImage image = frame.convertToFormat(QImage::Format_ARGB32);
    if (!settings.enabled())
        return image;
    fc::pixelate(image, settings.pixelBlock);
    fc::tintImage(image, settings.tint, 0.0);
    return image;
}
