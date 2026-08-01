#include "TabBar.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QProxyStyle>
#include <QRegion>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolButton>

#include "MotionPolicy.h"

namespace {
// A close button that paints its own "×" so the deepin (DTK) style can't
// recolour it the way it recolours QToolButton icons/stylesheet text.
class TabCloseButton : public QAbstractButton {
public:
    explicit TabCloseButton(QWidget *parent = nullptr) : QAbstractButton(parent) {
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
    }
    void setColour(const QColor &c) {
        if (m_colour != c) {
            m_colour = c;
            update();
        }
    }
    QSize sizeHint() const override { return QSize(18, 18); }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!isEnabled())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(underMouse() ? QColor(0xe0, 0x4b, 0x4b) : m_colour, 1.6));
        const int m = width() / 3;
        p.drawLine(m, m, width() - m, height() - m);
        p.drawLine(width() - m, m, m, height() - m);
    }

private:
    QColor m_colour = Qt::black;
};

class SplitTabScrollButtonStyle final : public QProxyStyle {
public:
    explicit SplitTabScrollButtonStyle(QStyle *baseStyle) : QProxyStyle(baseStyle) {}

    QRect subElementRect(SubElement element, const QStyleOption *option,
                         const QWidget *widget) const override {
        QRect rect = QProxyStyle::subElementRect(element, option, widget);
        if (!widget ||
            (element != QStyle::SE_TabBarScrollLeftButton &&
             element != QStyle::SE_TabBarScrollRightButton)) {
            return rect;
        }

        const int buttonWidth = qMax(1, rect.width());
        const int buttonHeight = rect.height() > 0 ? rect.height() : widget->height();
        const int y = qMax(0, (widget->height() - buttonHeight) / 2);
        if (element == QStyle::SE_TabBarScrollLeftButton)
            return QRect(0, y, buttonWidth, buttonHeight);
        return QRect(qMax(0, widget->width() - buttonWidth), y, buttonWidth, buttonHeight);
    }

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override {
        if (!qApp->styleSheet().isEmpty() &&
            (element == QStyle::CE_TabBarTab || element == QStyle::CE_TabBarTabShape ||
             element == QStyle::CE_TabBarTabLabel)) {
            qApp->style()->drawControl(element, option, painter, widget);
            return;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

} // namespace

TabBar::TabBar(QWidget *parent) : QTabBar(parent) {
    // Custom close buttons (see tabInserted) rather than the style's default,
    // so setTabsClosable stays off.
    setMovable(true);
    setExpanding(true);            // tabs stretch to fill the panel width
    setFocusPolicy(Qt::NoFocus);   // no dashed focus rectangle on the active tab
    QStyle *tabStyle = QStyleFactory::create(style()->objectName());
    if (!tabStyle)
        tabStyle = QStyleFactory::create(QStringLiteral("Fusion"));
    m_scrollButtonStyle = new SplitTabScrollButtonStyle(tabStyle);
    m_scrollButtonStyle->setParent(this);
    setStyle(m_scrollButtonStyle);

    connect(this, &QTabBar::tabCloseRequested, this, &TabBar::closeTabRequested);

    m_activationAnimation = new QPropertyAnimation(this, "activationProgress", this);
    connect(this, &QTabBar::currentChanged, this, [this](int) {
        animateCurrentTabActivation();
    });

}

QAbstractButton *TabBar::createCloseButton() {
    auto *close = new TabCloseButton(this);
    close->setToolTip(tr("Close Tab"));
    connect(close, &QAbstractButton::clicked, this, [this, close]() {
        for (int i = 0; i < count(); ++i) {
            if (tabButton(i, QTabBar::RightSide) == close) {
                emit tabCloseRequested(i);
                return;
            }
        }
    });
    return close;
}

void TabBar::tabInserted(int index) {
    QTabBar::tabInserted(index);
    setTabButton(index, QTabBar::RightSide, createCloseButton());
    arrangeScrollButtons();
}

void TabBar::tabRemoved(int index) {
    QTabBar::tabRemoved(index);
    arrangeScrollButtons();
}

void TabBar::resizeEvent(QResizeEvent *event) {
    QTabBar::resizeEvent(event);
    arrangeScrollButtons();
}

void TabBar::paintEvent(QPaintEvent *event) {
    arrangeScrollButtons();
    // Reconcile the close-button colours on every repaint: currentIndex() is
    // always authoritative here, so this self-corrects even when a tab was
    // added/switched with signals blocked. setColour() no-ops when unchanged,
    // so a settled tab bar does no extra work.
    refreshCloseButtons();
    QTabBar::paintEvent(event);

    // Mark the active tab with a thick accent line along its top edge, rather
    // than a filled background, so its label reads the same as the other tabs.
    const int current = currentIndex();
    QRegion scrollButtonRegion;
    for (QToolButton *button :
         findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if ((button->arrowType() == Qt::LeftArrow || button->arrowType() == Qt::RightArrow) &&
            button->isVisible()) {
            scrollButtonRegion += button->geometry();
        }
    }
    if (current >= 0) {
        const QRect r = tabRect(current);
        if (r.isValid()) {
            QPainter p(this);
            constexpr int thickness = 3;
            QColor accent = palette().color(QPalette::Highlight);
            accent.setAlphaF(accent.alphaF() * m_activationProgress);
            if (!scrollButtonRegion.isEmpty())
                p.setClipRegion(QRegion(r).subtracted(scrollButtonRegion));
            p.fillRect(r.x(), r.y(), r.width(), thickness, accent);
        }
    }

    if (!scrollButtonRegion.isEmpty()) {
        QPainter p(this);
        p.setClipRegion(scrollButtonRegion);
        const auto rects = scrollButtonRegion.rects();
        for (const QRect &rect : rects)
            p.fillRect(rect, palette().color(QPalette::Window));
    }

    arrangeScrollButtons();
}

void TabBar::setActivationProgress(qreal progress) {
    progress = qBound<qreal>(0.0, progress, 1.0);
    if (qFuzzyCompare(m_activationProgress, progress))
        return;
    m_activationProgress = progress;
    update();
}

void TabBar::animateCurrentTabActivation() {
    if (count() > 1 && currentIndex() >= 0)
        animateActivation();
    else
        setActivationProgress(1.0);
}

void TabBar::animateActivation() {
    const bool wasRunning =
        m_activationAnimation->state() == QAbstractAnimation::Running;
    const qreal start = wasRunning ? m_activationProgress : 0.0;
    m_activationAnimation->stop();

    const int duration = MotionPolicy::duration(MotionDuration::Fast);
    if (duration == 0) {
        setActivationProgress(1.0);
        return;
    }

    setActivationProgress(start);
    m_activationAnimation->setDuration(duration);
    m_activationAnimation->setEasingCurve(MotionPolicy::easing());
    m_activationAnimation->setStartValue(start);
    m_activationAnimation->setEndValue(1.0);
    m_activationAnimation->start();
}

void TabBar::refreshCloseButtons() {
    // A lone tab cannot be closed, but its disabled button stays in the layout.
    // Removing it changes QTabBar's height calculation and misaligns the two
    // panel rows when the other panel has multiple tabs.
    const bool multiple = count() > 1;
    const QColor normal = palette().color(QPalette::WindowText);
    for (int i = 0; i < count(); ++i) {
        QWidget *existing = tabButton(i, QTabBar::RightSide);
        if (!multiple) {
            if (!existing) {
                setTabButton(i, QTabBar::RightSide, createCloseButton());
                existing = tabButton(i, QTabBar::RightSide);
            }
            existing->setEnabled(false);
            continue;
        }
        if (!existing) {
            setTabButton(i, QTabBar::RightSide, createCloseButton());
            existing = tabButton(i, QTabBar::RightSide);
        }
        existing->setEnabled(true);
        static_cast<TabCloseButton *>(existing)->setColour(normal);
    }
}

void TabBar::arrangeScrollButtons() {
    QToolButton *left = nullptr;
    QToolButton *right = nullptr;
    for (QToolButton *button :
         findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button->arrowType() == Qt::LeftArrow)
            left = button;
        else if (button->arrowType() == Qt::RightArrow)
            right = button;
    }

    if (left && left->isVisible()) {
        left->setAutoFillBackground(true);
        left->setAttribute(Qt::WA_StyledBackground, true);
        left->setFocusPolicy(Qt::NoFocus);
        left->move(0, qMax(0, (height() - left->height()) / 2));
        left->raise();
    }
    if (right && right->isVisible()) {
        right->setAutoFillBackground(true);
        right->setAttribute(Qt::WA_StyledBackground, true);
        right->setFocusPolicy(Qt::NoFocus);
        right->move(qMax(0, width() - right->width()),
                    qMax(0, (height() - right->height()) / 2));
        right->raise();
    }
}

void TabBar::contextMenuEvent(QContextMenuEvent *event) {
    // Right-clicking anywhere on the tab strip opens the directory-favorites
    // menu (bookmark toggle for the current directory + the saved favorites,
    // synced with Ctrl+D). The panel/window owns the favorites list, so we
    // forward the location and the right-clicked tab (so a chosen favorite
    // navigates *that* tab, not merely the active one) and let it build the menu.
    emit favoritesMenuRequested(event->globalPos(), tabAt(event->pos()));
}
