#include "TabBar.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QToolButton>

TabBar::TabBar(QWidget *parent) : QTabBar(parent) {
    // Custom close buttons (see tabInserted) rather than the style's default,
    // so setTabsClosable stays off.
    setMovable(true);
    setExpanding(true);            // tabs stretch to fill the panel width
    setFocusPolicy(Qt::NoFocus);   // no dashed focus rectangle on the active tab

    connect(this, &QTabBar::tabCloseRequested, this, &TabBar::closeTabRequested);
    connect(this, &QTabBar::currentChanged, this, [this](int) { refreshCloseButtons(); });
}

void TabBar::tabInserted(int index) {
    QTabBar::tabInserted(index);

    // A plain "×" glyph -- no circular button background. Colour is set by
    // refreshCloseButtons() so it stays legible per tab state and theme.
    auto *close = new QToolButton(this);
    close->setText(QStringLiteral("×"));
    close->setFocusPolicy(Qt::NoFocus);
    close->setCursor(Qt::ArrowCursor);
    close->setToolTip(tr("Close Tab"));
    connect(close, &QToolButton::clicked, this, [this, close]() {
        for (int i = 0; i < count(); ++i) {
            if (tabButton(i, QTabBar::RightSide) == close) {
                emit tabCloseRequested(i);
                return;
            }
        }
    });
    setTabButton(index, QTabBar::RightSide, close);
    refreshCloseButtons();
}

void TabBar::refreshCloseButtons() {
    const QString normal = palette().color(QPalette::WindowText).name();
    const QString active = palette().color(QPalette::HighlightedText).name();
    for (int i = 0; i < count(); ++i) {
        auto *btn = qobject_cast<QToolButton *>(tabButton(i, QTabBar::RightSide));
        if (!btn)
            continue;
        const QString colour = (i == currentIndex()) ? active : normal;
        btn->setStyleSheet(
            QStringLiteral("QToolButton { border: none; background: transparent;"
                           " padding: 0 3px; font-size: 15px; font-weight: bold; color: %1; }")
                .arg(colour));
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
    menu.addAction(tr("Close Tabs to the Right"), this,
                    [this, index]() { emit closeToRightRequested(index); });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [this, index]() { emit copyPathRequested(index); });
    menu.exec(event->globalPos());
}
