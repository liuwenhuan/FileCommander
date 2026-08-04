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

void WindowsMediaSurface::setRotation(int degrees) {
    const int normalised = ((degrees % 360) + 360) % 360;
    if (normalised == m_rotation)
        return;
    m_rotation = normalised;
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

    // Fit the frame as the viewer will SEE it: at a quarter turn the clip's
    // width and height swap, so fitting the unrotated size would letterbox
    // against the wrong axis and leave a portrait clip cropped.
    const bool quarterTurn = m_rotation == 90 || m_rotation == 270;
    const QSize viewed = quarterTurn ? m_frame.size().transposed() : m_frame.size();
    const QSize targetSize = viewed.scaled(rect().size(), Qt::KeepAspectRatio);

    if (m_rotation == 0) {
        const QPoint topLeft((width() - targetSize.width()) / 2,
                             (height() - targetSize.height()) / 2);
        painter.drawImage(QRect(topLeft, targetSize), m_frame);
        return;
    }

    // Rotate about the centre of the widget, then draw the frame into the
    // pre-rotation rectangle -- which is the target with its axes swapped back.
    const QSize drawSize = quarterTurn ? targetSize.transposed() : targetSize;
    painter.translate(width() / 2.0, height() / 2.0);
    painter.rotate(m_rotation);
    painter.drawImage(QRect(QPoint(-drawSize.width() / 2, -drawSize.height() / 2), drawSize),
                      m_frame);
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
