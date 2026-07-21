#include "TitleBar.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWindow>

namespace {

// A minimize / maximize-restore / close button that paints its own glyph, so
// the deepin (DTK) style can't recolour or restyle it. Mirrors the approach
// used for the tab close buttons.
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

} // namespace

TitleBar::TitleBar(QWidget *window, const QList<QMenu *> &menus, QWidget *parent)
    : QWidget(parent), m_window(window) {
    setAutoFillBackground(false);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(2);

    // App icon (from the window's icon).
    auto *icon = new QLabel(this);
    const int iconSize = 18;
    icon->setPixmap(window->windowIcon().pixmap(iconSize, iconSize));
    icon->setFixedSize(iconSize + 6, iconSize + 4);
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);

    // Menu buttons (Commands / View): flat tool buttons that pop their menu.
    for (QMenu *menu : menus) {
        auto *btn = new QToolButton(this);
        btn->setText(menu->title());
        btn->setMenu(menu);
        btn->setPopupMode(QToolButton::InstantPopup);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // Hide the little dropdown arrow so it reads like a menu-bar entry.
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { padding: 4px 8px; } QToolButton::menu-indicator { image: none; }"));
        layout->addWidget(btn);
    }

    // Draggable empty area in the middle.
    layout->addStretch(1);

    auto *minButton = new TitleButton(TitleButton::Minimize, this);
    m_maxButton = new TitleButton(TitleButton::Maximize, this);
    auto *closeButton = new TitleButton(TitleButton::Close, this);
    QAbstractButton *winButtons[] = {minButton, m_maxButton, closeButton};
    for (auto *b : winButtons) {
        b->setFixedSize(46, 30);
        layout->addWidget(b);
    }
    connect(minButton, &QAbstractButton::clicked, this,
            [this] { m_window->showMinimized(); });
    connect(m_maxButton, &QAbstractButton::clicked, this, [this] {
        if (m_window->isMaximized())
            m_window->showNormal();
        else
            m_window->showMaximized();
        syncWindowState();
    });
    connect(closeButton, &QAbstractButton::clicked, this, [this] { m_window->close(); });

    setFixedHeight(30);
}

void TitleBar::syncWindowState() {
    if (m_maxButton)
        static_cast<TitleButton *>(m_maxButton)
            ->setKind(m_window->isMaximized() ? TitleButton::Restore : TitleButton::Maximize);
}

void TitleBar::mousePressEvent(QMouseEvent *event) {
    // Only remember the press here; starting the WM move immediately would eat
    // the event sequence and break double-click-to-maximize. The actual move is
    // kicked off on drag (mouseMoveEvent).
    if (event->button() == Qt::LeftButton)
        m_pressed = true;
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent *event) {
    if (m_pressed && (event->buttons() & Qt::LeftButton)) {
        m_pressed = false;
        if (QWindow *handle = m_window->windowHandle())
            handle->startSystemMove(); // WM-driven drag
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event) {
    m_pressed = false;
    QWidget::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (m_window->isMaximized())
            m_window->showNormal();
        else
            m_window->showMaximized();
        syncWindowState();
    }
}

void TitleBar::paintEvent(QPaintEvent *) {
    // Theme-following background: the window colour, a touch lighter/darker than
    // the panels so the bar reads as chrome.
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Window));
}
