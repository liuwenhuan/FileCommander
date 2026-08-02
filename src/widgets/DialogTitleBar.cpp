#include "DialogTitleBar.h"

#include "TitleButton.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

namespace {
constexpr int kDefaultHeight = 30;
constexpr int kVerticalTextPadding = 8;
}

DialogTitleBar::DialogTitleBar(QWidget *window, QWidget *parent)
    : QWidget(parent), m_window(window) {
    // Let qproperty-backgroundTile from the CRT stylesheet reach this custom
    // painted widget; flat themes retain the transparent stylesheet behaviour.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    // Translucent so the rounded top corners reveal the dialog's shadow/rounded
    // background behind them (the dialog is frameless; see FramelessDialog).
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(2);

    // App icon (from the window's icon), matching the main window's title bar.
    auto *icon = new QLabel(this);
    const int iconSize = 18;
    icon->setPixmap(window->windowIcon().pixmap(iconSize, iconSize));
    icon->setFixedSize(iconSize + 6, iconSize + 4);
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);

    layout->addStretch(1);

    auto *closeButton = new TitleButton(TitleButton::Close, this);
    closeButton->setFixedWidth(46);
    m_closeButton = closeButton;
    layout->addWidget(closeButton);
    connect(closeButton, &QAbstractButton::clicked, this, [this] { m_window->close(); });

    updateMetrics();
}

void DialogTitleBar::updateMetrics() {
    const int newHeight = qMax(kDefaultHeight, fontMetrics().height() + kVerticalTextPadding);
    if (m_closeButton)
        m_closeButton->setFixedHeight(newHeight);
    if (height() == newHeight && minimumHeight() == newHeight && maximumHeight() == newHeight)
        return;
    setFixedHeight(newHeight);
    emit heightChanged(newHeight);
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
        if (QWindow *handle = m_window->windowHandle())
            handle->startSystemMove(); // WM-driven drag
    }
    QWidget::mouseMoveEvent(event);
}

void DialogTitleBar::mouseReleaseEvent(QMouseEvent *event) {
    m_pressed = false;
    QWidget::mouseReleaseEvent(event);
}

void DialogTitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    // Dialogs are not maximizable; swallow the double-click so it doesn't leak
    // through to whatever sits behind the bar.
    QWidget::mouseDoubleClickEvent(event);
}

void DialogTitleBar::paintEvent(QPaintEvent *) {
    // Theme-following background with rounded top corners (radius 8, kept in sync
    // with FramelessDialog's corner radius).
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QBrush bg = m_backgroundTile.isNull()
                          ? QBrush(palette().color(QPalette::Window))
                          : QBrush(m_backgroundTile);
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
