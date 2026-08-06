#include "TitleBar.h"

#include "TitleButton.h"

#include <QAbstractButton>
#include <QEvent>
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

    // A transparent overlay keeps the title above the asymmetric controls while
    // its geometry remains the complete title-bar rectangle. Mouse events pass
    // through to the controls or the bar's window-drag handling underneath.
    m_title = new QLabel(this);
    m_title->setText(tr("FileCommander"));
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    positionTitle();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(2);

    // App icon (from the window's icon).
    m_icon = new QLabel(this);
    const int iconSize = 18;
    m_icon->setObjectName(QStringLiteral("ApplicationIcon"));
    m_icon->setFixedSize(iconSize + 6, iconSize + 4);
    m_icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_icon);
    syncWindowIcon();
    if (m_window)
        m_window->installEventFilter(this);

    // Menu buttons (Commands / View): flat tool buttons that pop their menu.
    for (QMenu *menu : menus) {
        auto *btn = new QToolButton(this);
        btn->setText(menu->title());
        btn->setMenu(menu);
        btn->setPopupMode(QToolButton::InstantPopup);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setObjectName(QStringLiteral("TitleMenuButton"));
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        layout->addWidget(btn);

        // Belt-and-braces alongside MainWindow::event()'s own guard against a
        // popup's closing mouse event replaying onto the frameless resize band: that
        // guard infers "a menu just closed here" from mouse-event timing, which is
        // fragile (observed to still leave the cursor stuck as a resize shape,
        // especially the very first time a menu is opened, before anything about
        // this popup's grab/replay sequence has been through it once already).
        // Resetting directly off the menu's own aboutToHide -- with no dependence on
        // event timing or where the mouse ends up -- can't miss.
        if (m_window) {
            connect(menu, &QMenu::aboutToHide, m_window, [window = m_window] {
                window->unsetCursor();
            });
        }
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
    positionTitle();
    m_title->raise();
}

void TitleBar::setBackgroundTile(const QPixmap &tile) {
    if (m_backgroundTile.cacheKey() == tile.cacheKey())
        return;
    m_backgroundTile = tile;
    update();
}

void TitleBar::positionTitle() {
    if (m_title)
        m_title->setGeometry(rect());
}

void TitleBar::setThemedIcon(const QIcon &icon) {
    m_themedIcon = icon;
    syncWindowIcon();
}

void TitleBar::syncWindowIcon() {
    if (!m_icon || !m_window)
        return;
    const int iconSize = qMin(m_icon->width() - 6, m_icon->height() - 4);
    // The themed mark when a theme has supplied one, and the window's own icon
    // before that -- which is what the very first paint uses.
    const QIcon &source = m_themedIcon.isNull() ? m_window->windowIcon() : m_themedIcon;
    m_icon->setPixmap(source.pixmap(iconSize, iconSize));
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::WindowIconChange)
        syncWindowIcon();
    return QWidget::eventFilter(watched, event);
}

void TitleBar::resizeEvent(QResizeEvent *event) {
    positionTitle();
    QWidget::resizeEvent(event);
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

}
