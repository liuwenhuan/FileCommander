#include "DragPixmap.h"

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QRectF>

namespace ttc {

QPixmap makeDragPixmap(const QIcon &firstIcon, int count, qreal dpr,
                       const QColor &accent, const QColor &accentText) {
    if (dpr <= 0)
        dpr = 1.0;
    const int s = 48; // logical icon edge

    // Front icon at device resolution, tagged with the dpr so the painter and
    // the drag layer treat it as `s` logical pixels.
    QPixmap icon = firstIcon.pixmap(QSize(s, s) * dpr);
    if (icon.isNull()) {
        icon = QPixmap(QSize(s, s) * dpr);
        icon.fill(Qt::transparent);
    }
    icon.setDevicePixelRatio(dpr);

    if (count <= 1)
        return icon;

    // Layout (logical px): the front icon sits at (pad, topRoom+pad); two copies
    // step down-right behind it, and the badge straddles the front icon's
    // top-right corner. The canvas reserves `topRoom` above and `pad` on every
    // side so neither the deepest copy nor the badge is ever clipped.
    const int off = 8;      // per-copy step (down-right)
    const int nBack = 2;    // copies behind the front icon
    const int badgeD = 20;  // badge diameter
    const int topRoom = badgeD / 2;
    const int pad = 3;

    const int stackExtent = s + off * nBack; // width/height spanned by the pile
    const QSize logical(stackExtent + pad * 2, topRoom + stackExtent + pad * 2);

    QPixmap canvas(logical * dpr);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int baseX = pad;
    const int baseY = topRoom + pad;

    // Stacked copies behind the front icon (deepest = faintest) suggest a pile.
    p.setOpacity(0.40);
    p.drawPixmap(QPoint(baseX + off * 2, baseY + off * 2), icon);
    p.setOpacity(0.70);
    p.drawPixmap(QPoint(baseX + off, baseY + off), icon);
    p.setOpacity(1.0);
    p.drawPixmap(QPoint(baseX, baseY), icon);

    // Count badge straddling the front icon's top-right corner.
    const qreal cx = baseX + s;
    const qreal cy = baseY;
    const QRectF badge(cx - badgeD / 2.0, cy - badgeD / 2.0, badgeD, badgeD);
    p.setPen(Qt::NoPen);
    p.setBrush(accent.isValid() ? accent : QColor(0x3d, 0x7d, 0xeb));
    p.drawEllipse(badge);
    p.setPen(accentText.isValid() ? accentText : QColor(Qt::white));
    QFont f = p.font();
    f.setBold(true);
    p.setFont(f);
    p.drawText(badge, Qt::AlignCenter, count > 99 ? QStringLiteral("99+") : QString::number(count));
    p.end();

    return canvas;
}

} // namespace ttc
