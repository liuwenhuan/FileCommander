#include "PhosphorEffect.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>

#include "theme/Phosphor.h"

namespace ttc {

PhosphorEffect::PhosphorEffect(const QColor &tint, QObject *parent)
    : QGraphicsEffect(parent), m_tint(tint) {}

void PhosphorEffect::draw(QPainter *painter) {
    QPoint offset;
    // Device coordinates: the source is rasterised at the painter's current
    // scale, so a zoomed-in slide is tinted at full resolution rather than
    // being tinted small and then magnified.
    const QPixmap source = sourcePixmap(Qt::DeviceCoordinates, &offset);
    if (source.isNull())
        return;
    if (!m_tint.isValid()) {
        painter->drawPixmap(offset, source);
        return;
    }

    QImage image = source.toImage();
    // Device coordinates above means the block is already in the right units:
    // the same value the thumbnails and icons use, applied to the same pixels.
    fc::pixelate(image, fc::contentPixelBlock());
    fc::tintImage(image, m_tint);

    // sourcePixmap() in DeviceCoordinates hands back an already-transformed
    // bitmap, so it must be blitted with the painter's transform reset --
    // otherwise the scene transform is applied a second time.
    const QTransform restore = painter->worldTransform();
    painter->setWorldTransform(QTransform());
    painter->drawImage(offset, image);
    painter->setWorldTransform(restore);
}

} // namespace ttc
