#include "FramelessDialog.h"

#include "DialogTitleBar.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWindow>

namespace {
// Kept in sync with MainWindow's chrome constants so dialogs and the main window
// share one look: translucent shadow band, corner radius, edge grab band, and
// the self-drawn title bar height.
constexpr int kShadowMargin = 16;
constexpr int kCornerRadius = 8;
constexpr int kResizeGrab = 8;
constexpr int kTitleH = 30;

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
} // namespace

FramelessDialog::FramelessDialog(QWidget *parent) : QDialog(parent) {
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_TranslucentBackground);

    // Reserve the shadow band on every side plus the title bar height at the top.
    // The subclass's layout(this) is laid out inside the resulting contentsRect,
    // so its content flows below the title bar automatically.
    setContentsMargins(kShadowMargin, kShadowMargin + kTitleH, kShadowMargin,
                       kShadowMargin);

    m_titleBar = new DialogTitleBar(this, this);
    m_titleBar->raise();
}

void FramelessDialog::ensureFrameCache() {
    const QColor bg = palette().color(QPalette::Window);
    if (!m_frameCache.isNull() && m_frameCacheColor == bg)
        return;
    m_frameCacheColor = bg;

    // Render the shadow + rounded frame ONCE at the smallest representative size;
    // paintEvent blits it 9-patch style (see MainWindow::ensureFrameCache).
    const int corner = kShadowMargin + kCornerRadius + 1;
    const int size = corner * 2 + 2;
    m_frameCache = QPixmap(size, size);
    m_frameCache.fill(Qt::transparent);

    QPainter p(&m_frameCache);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect content = QRect(0, 0, size, size)
                              .adjusted(kShadowMargin, kShadowMargin, -kShadowMargin,
                                        -kShadowMargin);
    p.setPen(Qt::NoPen);
    for (int i = kShadowMargin; i >= 1; --i) {
        const int alpha = 46 * (kShadowMargin - i + 1) / kShadowMargin;
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(QRectF(content).adjusted(-i, -i + 1, i, i + 1),
                          kCornerRadius + i, kCornerRadius + i);
    }
    p.setBrush(bg);
    p.drawRoundedRect(content, kCornerRadius, kCornerRadius);
}

void FramelessDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    ensureFrameCache();

    // 9-patch blit: four corners at 1:1, edges stretched along one axis, the
    // centre a solid fill.
    const int c = kShadowMargin + kCornerRadius + 1; // corner tile edge
    const int sw = m_frameCache.width();
    const int w = width(), h = height();
    const QPixmap &src = m_frameCache;

    p.drawPixmap(0, 0, src, 0, 0, c, c);
    p.drawPixmap(w - c, 0, src, sw - c, 0, c, c);
    p.drawPixmap(0, h - c, src, 0, sw - c, c, c);
    p.drawPixmap(w - c, h - c, src, sw - c, sw - c, c, c);
    p.drawPixmap(QRect(c, 0, w - 2 * c, c), src, QRect(c, 0, sw - 2 * c, c));
    p.drawPixmap(QRect(c, h - c, w - 2 * c, c), src, QRect(c, sw - c, sw - 2 * c, c));
    p.drawPixmap(QRect(0, c, c, h - 2 * c), src, QRect(0, c, c, sw - 2 * c));
    p.drawPixmap(QRect(w - c, c, c, h - 2 * c), src, QRect(sw - c, c, c, sw - 2 * c));
    p.fillRect(QRect(c, c, w - 2 * c, h - 2 * c), m_frameCacheColor);
}

void FramelessDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (m_titleBar)
        m_titleBar->setGeometry(kShadowMargin, kShadowMargin,
                                width() - 2 * kShadowMargin, kTitleH);
}

void FramelessDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    switch (event->type()) {
    case QEvent::WindowTitleChange:
        if (m_titleBar)
            m_titleBar->update();
        break;
    case QEvent::PaletteChange:
    case QEvent::ThemeChange:
        // Theme flipped: the cached frame was rendered in the old window colour.
        m_frameCache = QPixmap();
        update();
        break;
    default:
        break;
    }
}

bool FramelessDialog::event(QEvent *event) {
    // Frameless edge resize: the thin shadow band around the content reaches this
    // handler. Show the resize cursor on hover and hand off to the WM on press.
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QRect content =
            rect().adjusted(kShadowMargin, kShadowMargin, -kShadowMargin, -kShadowMargin);
        const Qt::Edges edges = edgesAt(content, me->pos());
        if (event->type() == QEvent::MouseMove) {
            if (me->buttons() == Qt::NoButton) {
                if (edges != Qt::Edges())
                    setCursor(cursorForEdges(edges));
                else
                    unsetCursor();
            }
        } else if (edges != Qt::Edges() && me->button() == Qt::LeftButton) {
            if (QWindow *handle = windowHandle()) {
                handle->startSystemResize(edges);
                unsetCursor();
                return true;
            }
        }
    }
    return QDialog::event(event);
}
