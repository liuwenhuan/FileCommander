#include "FramelessChrome.h"

#include <QPainter>

namespace ttc::chrome {

Qt::Edges edgesAt(const QRect &content, const QPoint &p) {
    Qt::Edges e;
    if (qAbs(p.x() - content.left()) <= kResizeGrab)
        e |= Qt::LeftEdge;
    else if (qAbs(p.x() - content.right()) <= kResizeGrab)
        e |= Qt::RightEdge;
    if (qAbs(p.y() - content.top()) <= kResizeGrab)
        e |= Qt::TopEdge;
    else if (qAbs(p.y() - content.bottom()) <= kResizeGrab)
        e |= Qt::BottomEdge;
    return e;
}

Qt::CursorShape cursorForEdges(Qt::Edges e) {
    const bool l = e & Qt::LeftEdge, r = e & Qt::RightEdge;
    const bool t = e & Qt::TopEdge, b = e & Qt::BottomEdge;
    if ((l && t) || (r && b))
        return Qt::SizeFDiagCursor;
    if ((r && t) || (l && b))
        return Qt::SizeBDiagCursor;
    if (l || r)
        return Qt::SizeHorCursor;
    if (t || b)
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

QPixmap renderFrameTile(const QColor &background, const QPixmap &tile) {
    // Corner tiles must span the shadow margin + corner radius; one extra
    // pixel row/column in the middle stretches cleanly (all mid-frame rows are
    // identical).
    const int corner = kShadowMargin + kCornerRadius + 1;
    const int size = corner * 2 + 2;
    QPixmap frame(size, size);
    frame.fill(Qt::transparent);

    QPainter p(&frame);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect content = QRect(0, 0, size, size)
                              .adjusted(kShadowMargin, kShadowMargin, -kShadowMargin,
                                        -kShadowMargin);
    p.setPen(Qt::NoPen);
    for (int i = kShadowMargin; i >= 1; --i) {
        const int alpha = 46 * (kShadowMargin - i + 1) / kShadowMargin;
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(QRectF(content).adjusted(-i, -i + 1, i, i + 1), kCornerRadius + i,
                          kCornerRadius + i);
    }
    p.setBrush(tile.isNull() ? QBrush(background) : QBrush(tile));
    p.drawRoundedRect(content, kCornerRadius, kCornerRadius);
    return frame;
}

void blitFrame(QPainter &p, const QSize &size, const QPixmap &frameTile, const QBrush &centre) {
    if (frameTile.isNull())
        return;
    const int c = kShadowMargin + kCornerRadius + 1; // corner tile edge
    const int sw = frameTile.width();
    const int w = size.width(), h = size.height();
    const QPixmap &src = frameTile;

    p.drawPixmap(0, 0, src, 0, 0, c, c);
    p.drawPixmap(w - c, 0, src, sw - c, 0, c, c);
    p.drawPixmap(0, h - c, src, 0, sw - c, c, c);
    p.drawPixmap(w - c, h - c, src, sw - c, sw - c, c, c);
    p.drawPixmap(QRect(c, 0, w - 2 * c, c), src, QRect(c, 0, sw - 2 * c, c));
    p.drawPixmap(QRect(c, h - c, w - 2 * c, c), src, QRect(c, sw - c, sw - 2 * c, c));
    p.drawPixmap(QRect(0, c, c, h - 2 * c), src, QRect(0, c, c, sw - 2 * c));
    p.drawPixmap(QRect(w - c, c, c, h - 2 * c), src, QRect(sw - c, c, c, sw - 2 * c));
    // Centre (a solid fill or the theme's tile; cheaper than a stretched blit).
    p.fillRect(QRect(c, c, w - 2 * c, h - 2 * c), centre);
}

} // namespace ttc::chrome
