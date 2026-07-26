#include "TitleBar.h"

#include "TitleButton.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QWindow>

TitleBar::TitleBar(QWidget *window, const QList<QMenu *> &menus, QWidget *parent)
    : QWidget(parent), m_window(window) {
    // A plain QWidget subclass does not paint a stylesheet background unless
    // it is told to; without this the CRT theme's scanline texture stops at
    // the Qt-provided widgets and this one stays flat. light.qss/dark.qss
    // declare `background: transparent` for this class so their appearance is
    // unchanged -- it shows the parent, exactly as it did before.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    // Translucent so the rounded top corners reveal the window's shadow/rounded
    // background behind them (the window is frameless, xcb; see MainWindow).
    setAttribute(Qt::WA_TranslucentBackground);

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

    // "New Version" badge: hidden until the update checker reports a release.
    // Sits just before the window buttons; clicking it opens the update dialog.
    auto *badge = new QToolButton(this);
    badge->setText(tr("New Version"));
    badge->setToolButtonStyle(Qt::ToolButtonTextOnly);
    badge->setAutoRaise(true);
    badge->setFocusPolicy(Qt::NoFocus);
    badge->setCursor(Qt::PointingHandCursor);
    badge->setObjectName("UpdateBadge");
    badge->setStyleSheet(QStringLiteral(
        "QToolButton#UpdateBadge { color: palette(highlight); font-weight: bold;"
        " padding: 2px 10px; }"));
    badge->hide();
    connect(badge, &QAbstractButton::clicked, this, [this] { emit updateRequested(); });
    layout->addWidget(badge);
    m_updateBadge = badge;

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

void TitleBar::setUpdateAvailable(bool available) {
    if (m_updateBadge)
        m_updateBadge->setVisible(available);
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
    // the panels so the bar reads as chrome. The top corners are rounded to
    // match the frameless window (radius 8, kept in sync with MainWindow's
    // kCornerRadius); a maximized window is square.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // A tile, when the theme supplied one, else the flat window colour.
    const QBrush bg = m_backgroundTile.isNull()
                          ? QBrush(palette().color(QPalette::Window))
                          : QBrush(m_backgroundTile);
    if (m_window && m_window->isMaximized()) {
        p.fillRect(rect(), bg);
    } else {
        constexpr int radius = 8;
        QPainterPath path;
        // Rounded top-left / top-right, square bottom (the body continues below).
        path.moveTo(0, height());
        path.lineTo(0, radius);
        path.arcTo(0, 0, 2 * radius, 2 * radius, 180, -90);
        path.lineTo(width() - radius, 0);
        path.arcTo(width() - 2 * radius, 0, 2 * radius, 2 * radius, 90, -90);
        path.lineTo(width(), height());
        path.closeSubpath();
        p.fillPath(path, bg);
    }

    // Centred application name, dimmed so it reads as chrome rather than a
    // control. Centred on the full bar width; the menu buttons on the left and
    // window buttons on the right are short enough not to overlap it.
    QColor fg = palette().color(QPalette::WindowText);
    fg.setAlpha(150);
    p.setPen(fg);
    // Center on the text's own ascent/descent rather than the full line box:
    // Qt::AlignCenter uses the line height (including leading) and leaves the ink
    // sitting a couple pixels high. Compute the baseline so it's optically centred.
    const QString title = tr("FileCommander");
    const QFontMetrics fm(p.font());
    const int x = (width() - fm.horizontalAdvance(title)) / 2;
    const int y = (height() + fm.ascent() - fm.descent()) / 2;
    p.drawText(x, y, title);
}
