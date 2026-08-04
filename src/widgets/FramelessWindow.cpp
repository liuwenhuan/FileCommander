#include "FramelessWindow.h"

#include "FramelessChrome.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWindow>

using namespace ttc::chrome;

FramelessWindow::FramelessWindow(QWidget *parent) : QWidget(parent) {
    // Qt::Window keeps this a real top-level window (taskbar / alt-tab entry);
    // the button hints keep the WM's own minimize and maximize affordances --
    // and, on Windows, the snap gestures -- available even though we draw the
    // decorations ourselves.
    setWindowFlags(windowFlags() | Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    setAttribute(Qt::WA_TranslucentBackground);
    // Hover has to reach event() for the edge-resize cursor; without tracking,
    // mouse moves are only delivered while a button is held.
    setMouseTracking(true);

    m_titleBar = new DialogTitleBar(this, this, DialogTitleBar::WindowControls);
    connect(m_titleBar, &DialogTitleBar::heightChanged, this,
            [this] { updateTitleBarLayout(); });
    updateTitleBarLayout();
    m_titleBar->raise();
}

void FramelessWindow::updateTitleBarLayout() {
    // Maximized: no shadow margin, so nothing transparent shows at the screen
    // edges (MainWindow::changeEvent does the same).
    const int margin = isMaximized() ? 0 : kShadowMargin;
    const int titleHeight = m_titleBar ? m_titleBar->height() : 0;
    setContentsMargins(margin, margin + titleHeight, margin, margin);
    if (m_titleBar)
        m_titleBar->setGeometry(margin, margin, qMax(0, width() - 2 * margin), titleHeight);
}

void FramelessWindow::setBackgroundTile(const QPixmap &tile) {
    if (m_backgroundTile.cacheKey() == tile.cacheKey())
        return;
    m_backgroundTile = tile;
    m_frameCache = QPixmap();
    if (m_titleBar)
        m_titleBar->setBackgroundTile(tile);
    update();
}

void FramelessWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QColor bg = palette().color(QPalette::Window);
    const QBrush body = m_backgroundTile.isNull() ? QBrush(bg) : QBrush(m_backgroundTile);

    if (isMaximized() || contentsMargins().left() == 0) {
        p.fillRect(rect(), body);
        return;
    }
    if (m_frameCache.isNull() || m_frameCacheColor != bg) {
        m_frameCacheColor = bg;
        m_frameCache = renderFrameTile(bg, m_backgroundTile);
    }
    blitFrame(p, size(), m_frameCache, body);
}

void FramelessWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateTitleBarLayout();
}

void FramelessWindow::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
        if (m_titleBar && m_titleBar->font() != font())
            m_titleBar->setFont(font());
        break;
    case QEvent::WindowStateChange:
        if (m_titleBar)
            m_titleBar->syncWindowState();
        updateTitleBarLayout();
        update();
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

bool FramelessWindow::event(QEvent *event) {
    // Frameless edge resize: the thin shadow band around the content reaches
    // this handler. Show the resize cursor on hover, hand off to the WM on
    // press. A maximized window has no band and must not resize.
    if (!isMaximized() &&
        (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress)) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QRect content =
            rect().adjusted(kShadowMargin, kShadowMargin, -kShadowMargin, -kShadowMargin);
        const Qt::Edges edges = edgesAt(content, me->pos());
        if (event->type() == QEvent::MouseMove) {
            if (me->buttons() == Qt::NoButton) {
                // Off the edge we UNSET rather than force Arrow: setCursor() on
                // the window is inherited by every child that has none of its
                // own, so forcing it would override theirs.
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
    return QWidget::event(event);
}
