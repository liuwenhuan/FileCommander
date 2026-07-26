#pragma once

#include <QColor>
#include <QGraphicsEffect>

namespace ttc {

// Applies fc::tintImage() to whatever it is attached to, for the one surface
// that is not a single bitmap: an Office slide is a QGraphicsScene of vector
// items and embedded images, so there is no pixmap to recolour on the way in.
//
// Qt already ships QGraphicsColorizeEffect, which also greys-then-tints, but its
// grey step uses qGray()'s weights (0.344/0.500/0.156) rather than BT.601
// (0.299/0.587/0.114). That is a visible difference on saturated reds and
// greens, and it would put slides half a shade away from the thumbnails and
// video sitting next to them. One transform for every surface is the whole
// point, so this does the conversion itself.
class PhosphorEffect : public QGraphicsEffect {
    Q_OBJECT

public:
    explicit PhosphorEffect(const QColor &tint, QObject *parent = nullptr);

protected:
    void draw(QPainter *painter) override;

private:
    QColor m_tint;
};

} // namespace ttc
