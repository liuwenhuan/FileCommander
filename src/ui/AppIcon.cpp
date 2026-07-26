#include "AppIcon.h"

#include <QPainter>
#include <QPixmap>

#include "theme/Phosphor.h"

namespace ttc {
namespace {

QPixmap paintIcon(int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size / 64.0;
    p.scale(s, s);
    p.setPen(Qt::NoPen);

    // Rounded body + darker title strip.
    p.setBrush(QColor(0x2c, 0x7b, 0xe5));
    p.drawRoundedRect(4, 8, 56, 48, 8, 8);
    p.setBrush(QColor(0x1b, 0x5f, 0xc0));
    p.drawRoundedRect(4, 8, 56, 16, 8, 8);
    p.drawRect(4, 16, 56, 8); // square off the strip's lower edge

    // Two panes with a few "file" rows, echoing the dual-pane layout.
    const QColor rowColor(0x9c, 0xc0, 0xf0);
    for (int pane = 0; pane < 2; ++pane) {
        const int x = 9 + pane * 26;
        p.setBrush(pane == 0 ? Qt::white : QColor(0xdb, 0xe7, 0xfb));
        p.drawRoundedRect(x, 28, 20, 24, 3, 3);
        p.setBrush(rowColor);
        for (int row = 0; row < 3; ++row)
            p.drawRect(x + 4, 33 + row * 5, 12, 2);
    }
    p.end();
    return pm;
}

} // namespace

QIcon appIcon(const QColor &tint) {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64}) {
        // Tinted but deliberately NOT quantised (block 0). The title bar draws
        // this about 20 pixels wide; on that budget any grid at all costs more
        // legibility than it buys atmosphere -- the two panes merge into one
        // blob. The artwork is painted in the original blue either way and
        // mapped by brightness, so the strip stays darker than the body and the
        // panes stay the brightest thing on it.
        icon.addPixmap(fc::tintedPixmap(paintIcon(size), tint, /*blockPixels=*/0));
    }
    return icon;
}

} // namespace ttc
