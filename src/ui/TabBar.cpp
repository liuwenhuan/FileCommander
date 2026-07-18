#include "TabBar.h"

#include <QContextMenuEvent>
#include <QMenu>

TabBar::TabBar(QWidget *parent) : QTabBar(parent) {
    setTabsClosable(true);
    setMovable(true);
    setExpanding(false);
    setFocusPolicy(Qt::ClickFocus); // keep out of the panel Tab-focus chain

    connect(this, &QTabBar::tabCloseRequested, this, &TabBar::closeTabRequested);
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
