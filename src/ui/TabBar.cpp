#include "TabBar.h"

#include <QAbstractButton>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QToolButton>

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
} // namespace

TabBar::TabBar(QWidget *parent) : QTabBar(parent) {
    // Custom close buttons (see tabInserted) rather than the style's default,
    // so setTabsClosable stays off.
    setMovable(true);
    setExpanding(true);            // tabs stretch to fill the panel width
    setFocusPolicy(Qt::NoFocus);   // no dashed focus rectangle on the active tab

    connect(this, &QTabBar::tabCloseRequested, this, &TabBar::closeTabRequested);

    // The left/right scroll buttons QTabBar creates for overflow are QToolButtons
    // whose arrows the style draws too large. Flatten them and take over their
    // painting (see eventFilter). They exist from construction, before any tab.
    const auto scrollButtons = findChildren<QToolButton *>();
    for (QToolButton *b : scrollButtons) {
        b->setAutoRaise(true);
        b->installEventFilter(this);
    }
}

bool TabBar::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Paint) {
        if (auto *btn = qobject_cast<QToolButton *>(watched)) {
            QPainter p(btn);
            p.setRenderHint(QPainter::Antialiasing);
            const QRect r = btn->rect();

            // DTK paints its (oversized) scroller arrow onto the tab bar itself,
            // beneath these transparent buttons -- so fill the button with the
            // tab-bar background first to hide it, then draw only our chevron.
            p.fillRect(r, palette().color(QPalette::Window));

            // Subtle hover fill, theme-adaptive, echoing the flat tool buttons.
            if (btn->isEnabled() && btn->underMouse()) {
                QColor hover = palette().color(QPalette::WindowText);
                hover.setAlpha(28);
                p.setPen(Qt::NoPen);
                p.setBrush(hover);
                p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 3, 3);
            }

            const QColor fg = btn->isEnabled() ? palette().color(QPalette::WindowText)
                                               : palette().color(QPalette::Mid);
            p.setPen(QPen(fg, 1.6));
            p.setBrush(Qt::NoBrush);
            const int cx = r.center().x();
            const int cy = r.center().y();
            const int aw = 3; // chevron half-width
            const int ah = 5; // chevron half-height
            // The style leaves arrowType() == NoArrow on these, so decide the
            // direction from the well-known object names instead.
            if (btn->objectName() == QLatin1String("ScrollLeftButton")) {
                p.drawLine(cx + aw, cy - ah, cx - aw, cy);
                p.drawLine(cx - aw, cy, cx + aw, cy + ah);
            } else {
                p.drawLine(cx - aw, cy - ah, cx + aw, cy);
                p.drawLine(cx + aw, cy, cx - aw, cy + ah);
            }
            return true; // painted; skip the style's default arrow
        }
    }
    return QTabBar::eventFilter(watched, event);
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
}

void TabBar::paintEvent(QPaintEvent *event) {
    // Reconcile the close-button colours on every repaint: currentIndex() is
    // always authoritative here, so this self-corrects even when a tab was
    // added/switched with signals blocked. setColour() no-ops when unchanged,
    // so a settled tab bar does no extra work.
    refreshCloseButtons();
    QTabBar::paintEvent(event);
}

void TabBar::refreshCloseButtons() {
    // A lone tab can't be closed, so it shows no × at all. With 2+ tabs, the
    // selected tab (blue in both themes) gets a white ×; others match the tab
    // label colour (palette WindowText) so the × is as legible as the text.
    const bool multiple = count() > 1;
    const QColor normal = palette().color(QPalette::WindowText);
    for (int i = 0; i < count(); ++i) {
        QWidget *existing = tabButton(i, QTabBar::RightSide);
        if (!multiple) {
            if (existing)
                setTabButton(i, QTabBar::RightSide, nullptr); // drop the × entirely
            continue;
        }
        if (!existing) {
            setTabButton(i, QTabBar::RightSide, createCloseButton());
            existing = tabButton(i, QTabBar::RightSide);
        }
        static_cast<TabCloseButton *>(existing)->setColour(i == currentIndex() ? QColor(Qt::white)
                                                                              : normal);
    }
}

void TabBar::contextMenuEvent(QContextMenuEvent *event) {
    const int index = tabAt(event->pos());
    if (index < 0) {
        QMenu menu(this);
        menu.addAction(tr("New Tab"), this, &TabBar::newTabRequested);
        menu.exec(event->globalPos());
        return;
    }

    QMenu menu(this);
    menu.addAction(tr("New Tab"), this, &TabBar::newTabRequested);
    menu.addAction(tr("Close Tab"), this, [this, index]() { emit closeTabRequested(index); });
    menu.addAction(tr("Close Others"), this,
                    [this, index]() { emit closeOthersRequested(index); });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [this, index]() { emit copyPathRequested(index); });
    menu.exec(event->globalPos());
}
