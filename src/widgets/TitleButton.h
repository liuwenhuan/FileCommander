#pragma once

#include <QAbstractButton>
#include <QPainter>
#include <QPalette>

// A minimize / maximize-restore / close button that paints its own glyph, so
// the deepin (DTK) style can't recolour or restyle it. Shared by the main
// window's TitleBar and the frameless dialog title bar (DialogTitleBar).
//
// Header-only: every method is defined in the class body (implicitly inline),
// so it can be included in multiple translation units without ODR issues.
class TitleButton : public QAbstractButton {
public:
    enum Kind { Minimize, Maximize, Restore, Close };

    explicit TitleButton(Kind kind, QWidget *parent = nullptr)
        : QAbstractButton(parent), m_kind(kind) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
    }
    void setKind(Kind kind) {
        if (m_kind != kind) {
            m_kind = kind;
            update();
        }
    }
    QSize sizeHint() const override { return QSize(46, 30); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hover = underMouse();

        // Hover background: red for close, a subtle tint for the others.
        if (hover) {
            if (m_kind == Close)
                p.fillRect(rect(), QColor(0xe8, 0x11, 0x23));
            else {
                QColor tint = palette().color(QPalette::WindowText);
                tint.setAlpha(30);
                p.fillRect(rect(), tint);
            }
        }

        QColor fg = (m_kind == Close && hover) ? QColor(Qt::white)
                                               : palette().color(QPalette::WindowText);
        p.setPen(QPen(fg, 1.3));
        const QPointF c = rect().center() + QPointF(0.5, 0.5);
        const int r = 5; // glyph half-size

        switch (m_kind) {
        case Minimize:
            p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
            break;
        case Maximize:
            p.drawRect(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r));
            break;
        case Restore: {
            // Two offset squares for the "restore" state.
            const qreal o = 2.5;
            p.drawRect(QRectF(c.x() - r + o, c.y() - r - o + 1, 2 * r - o, 2 * r - o));
            p.fillRect(QRectF(c.x() - r - o + 1, c.y() - r + o, 2 * r - o, 2 * r - o),
                       palette().color(QPalette::Window));
            p.drawRect(QRectF(c.x() - r - o + 1, c.y() - r + o, 2 * r - o, 2 * r - o));
            break;
        }
        case Close:
            p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
            p.drawLine(QPointF(c.x() + r, c.y() - r), QPointF(c.x() - r, c.y() + r));
            break;
        }
    }

private:
    Kind m_kind;
};
