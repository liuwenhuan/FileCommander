#pragma once

#include <QTabBar>

// Visual tab strip for one FilePanel. Thin wrapper around QTabBar adding
// the TC-style right-click menu (new/close/close others/close right/copy
// path). Tab state itself lives in TabManager, not here.
class TabBar : public QTabBar {
    Q_OBJECT

public:
    explicit TabBar(QWidget *parent = nullptr);

signals:
    void newTabRequested();
    void closeTabRequested(int index);
    void closeOthersRequested(int index);
    void closeToRightRequested(int index);
    void copyPathRequested(int index);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
};
