#include "DialogTitleBar.h"

#include "TitleButton.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

namespace {
constexpr int kDefaultHeight = 30;
constexpr int kVerticalTextPadding = 8;
constexpr int kButtonWidth = 46;
constexpr int kIconSize = 18;
} // namespace

DialogTitleBar::DialogTitleBar(QWidget *window, QWidget *parent, Controls controls)
    : QWidget(parent), m_window(window), m_controls(controls) {
    // Let qproperty-backgroundTile from the CRT stylesheet reach this custom
    // painted widget; flat themes retain the transparent stylesheet behaviour.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    // Translucent so the rounded top corners reveal the host's shadow/rounded
    // background behind them (the host is frameless; see FramelessDialog and
    // FramelessWindow).
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(2);

    // App icon (from the window's icon), matching the main window's title bar.
    m_icon = new QLabel(this);
    m_icon->setFixedSize(kIconSize + 6, kIconSize + 4);
    m_icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_icon);
    syncWindowIcon();
    // The app icon is repainted (not recoloured in place) on a theme change and
    // pushed onto every top-level window, so the bar has to hear about it --
    // and about a title change, which is what makes the F4 editor's " *"
    // modified marker show up.
    if (m_window)
        m_window->installEventFilter(this);

    layout->addStretch(1);

    if (m_controls == WindowControls) {
        auto *minButton = new TitleButton(TitleButton::Minimize, this);
        minButton->setFixedWidth(kButtonWidth);
        m_minButton = minButton;
        layout->addWidget(minButton);
        connect(minButton, &QAbstractButton::clicked, this, [this] {
            if (m_window)
                m_window->showMinimized();
        });

        auto *maxButton = new TitleButton(TitleButton::Maximize, this);
        maxButton->setFixedWidth(kButtonWidth);
        m_maxButton = maxButton;
        layout->addWidget(maxButton);
        connect(maxButton, &QAbstractButton::clicked, this, [this] { toggleMaximized(); });
    }

    auto *closeButton = new TitleButton(TitleButton::Close, this);
    closeButton->setFixedWidth(kButtonWidth);
    m_closeButton = closeButton;
    layout->addWidget(closeButton);
    connect(closeButton, &QAbstractButton::clicked, this, [this] {
        if (m_window)
            m_window->close();
    });

    updateMetrics();
    syncWindowState();
}

void DialogTitleBar::updateMetrics() {
    const int newHeight = qMax(kDefaultHeight, fontMetrics().height() + kVerticalTextPadding);
    for (QAbstractButton *b : {m_minButton, m_maxButton, m_closeButton}) {
        if (b)
            b->setFixedHeight(newHeight);
    }
    if (height() == newHeight && minimumHeight() == newHeight && maximumHeight() == newHeight)
        return;
    setFixedHeight(newHeight);
    emit heightChanged(newHeight);
}

void DialogTitleBar::setThemedIcon(const QIcon &icon) {
    m_themedIcon = icon;
    syncWindowIcon();
}

void DialogTitleBar::syncWindowIcon() {
    if (!m_icon || !m_window)
        return;
    const QIcon &source = m_themedIcon.isNull() ? m_window->windowIcon() : m_themedIcon;
    m_icon->setPixmap(source.pixmap(kIconSize, kIconSize));
}

void DialogTitleBar::syncWindowState() {
    if (!m_maxButton || !m_window)
        return;
    static_cast<TitleButton *>(m_maxButton)
        ->setKind(m_window->isMaximized() ? TitleButton::Restore : TitleButton::Maximize);
}

void DialogTitleBar::toggleMaximized() {
    if (!m_window)
        return;
    if (m_window->isMaximized())
        m_window->showNormal();
    else
        m_window->showMaximized();
    syncWindowState();
}

bool DialogTitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window) {
        if (event->type() == QEvent::WindowIconChange)
            syncWindowIcon();
        else if (event->type() == QEvent::WindowTitleChange)
            update();
    }
    return QWidget::eventFilter(watched, event);
}

void DialogTitleBar::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        updateMetrics();
}

void DialogTitleBar::setBackgroundTile(const QPixmap &tile) {
    if (m_backgroundTile.cacheKey() == tile.cacheKey())
        return;
    m_backgroundTile = tile;
    update();
}

void DialogTitleBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        m_pressed = true;
    QWidget::mousePressEvent(event);
}

void DialogTitleBar::mouseMoveEvent(QMouseEvent *event) {
    if (m_pressed && (event->buttons() & Qt::LeftButton)) {
        m_pressed = false;
        // Only remember the press above; starting the WM move on press would
        // eat the event sequence and break double-click-to-maximize. Handing the
        // drag to the WM (rather than moving the window ourselves) is also what
        // keeps the snap gestures working -- same as MainWindow's TitleBar.
        if (m_window) {
            if (QWindow *handle = m_window->windowHandle())
                handle->startSystemMove();
        }
    }
    QWidget::mouseMoveEvent(event);
}

void DialogTitleBar::mouseReleaseEvent(QMouseEvent *event) {
    m_pressed = false;
    QWidget::mouseReleaseEvent(event);
}

void DialogTitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    // A window maximizes; a dialog is not maximizable, so the double-click is
    // swallowed there rather than leaking through to whatever sits behind.
    if (m_controls == WindowControls && event->button() == Qt::LeftButton) {
        toggleMaximized();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void DialogTitleBar::paintEvent(QPaintEvent *) {
    // Theme-following background with rounded top corners (radius 8, kept in
    // sync with the host's corner radius). A maximized window is square, since
    // its shadow margin is collapsed and there is no rounding left to match.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QBrush bg = m_backgroundTile.isNull()
                          ? QBrush(palette().color(QPalette::Window))
                          : QBrush(m_backgroundTile);
    if (m_window && m_window->isMaximized()) {
        p.fillRect(rect(), bg);
    } else {
        constexpr int radius = 8;
        QPainterPath path;
        path.moveTo(0, height());
        path.lineTo(0, radius);
        path.arcTo(0, 0, 2 * radius, 2 * radius, 180, -90);
        path.lineTo(width() - radius, 0);
        path.arcTo(width() - 2 * radius, 0, 2 * radius, 2 * radius, 90, -90);
        path.lineTo(width(), height());
        path.closeSubpath();
        p.fillPath(path, bg);
    }

    // Centred window title, dimmed so it reads as chrome. Optically centred on
    // the text's ascent/descent (see TitleBar for the rationale).
    QColor fg = palette().color(QPalette::WindowText);
    fg.setAlpha(150);
    p.setPen(fg);
    const QString title = m_window ? m_window->windowTitle() : QString();
    const QFontMetrics fm(p.font());
    const int x = (width() - fm.horizontalAdvance(title)) / 2;
    const int y = (height() + fm.ascent() - fm.descent()) / 2;
    p.drawText(x, y, title);
}
