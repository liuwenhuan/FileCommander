#include "FramelessDialog.h"

#include "DialogTitleBar.h"
#include "FramelessChrome.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWindow>

using namespace ttc::chrome;

FramelessDialog::FramelessDialog(QWidget *parent) : QDialog(parent) {
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_TranslucentBackground);

    m_titleBar = new DialogTitleBar(this, this);
    connect(m_titleBar, &DialogTitleBar::heightChanged, this,
            [this] { updateTitleBarLayout(); });
    updateTitleBarLayout();
    m_titleBar->raise();
}

void FramelessDialog::updateTitleBarLayout() {
    const int titleHeight = m_titleBar ? m_titleBar->height() : 0;
    setContentsMargins(kShadowMargin, kShadowMargin + titleHeight, kShadowMargin,
                       kShadowMargin);
    if (m_titleBar)
        m_titleBar->setGeometry(kShadowMargin, kShadowMargin,
                                qMax(0, width() - 2 * kShadowMargin), titleHeight);
}

void FramelessDialog::setBackgroundTile(const QPixmap &tile) {
    if (m_backgroundTile.cacheKey() == tile.cacheKey())
        return;
    m_backgroundTile = tile;
    m_frameCache = QPixmap();
    if (m_titleBar)
        m_titleBar->setBackgroundTile(tile);
    update();
}

void FramelessDialog::ensureFrameCache() {
    const QColor bg = palette().color(QPalette::Window);
    if (!m_frameCache.isNull() && m_frameCacheColor == bg)
        return;
    m_frameCacheColor = bg;
    m_frameCache = renderFrameTile(bg, m_backgroundTile);
}

void FramelessDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    ensureFrameCache();
    blitFrame(p, size(), m_frameCache,
              m_backgroundTile.isNull() ? QBrush(m_frameCacheColor) : QBrush(m_backgroundTile));
}

void FramelessDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    updateTitleBarLayout();
}

void FramelessDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
        if (m_titleBar && m_titleBar->font() != font())
            m_titleBar->setFont(font());
        break;
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
