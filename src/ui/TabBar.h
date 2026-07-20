#pragma once

#include <QTabBar>

class QAbstractButton;

// Visual tab strip for one FilePanel. Thin wrapper around QTabBar adding
// the TC-style right-click menu (new/close/close others/copy path). Tab
// state itself lives in TabManager, not here.
class TabBar : public QTabBar {
    Q_OBJECT

public:
    explicit TabBar(QWidget *parent = nullptr);

signals:
    void newTabRequested();
    void closeTabRequested(int index);
    void closeOthersRequested(int index);
    void copyPathRequested(int index);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void tabInserted(int index) override;
    void paintEvent(QPaintEvent *event) override;
    // Repaints the style's oversized tab-scroll arrows (shown when tabs
    // overflow) as small flat chevrons matching the panel's other buttons.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Keeps the "×" buttons in sync: they all use the tab label colour (palette
    // WindowText) for legibility, and a lone tab shows none at all. Driven from
    // paintEvent so it always sees the correct tab count, even when tabs are
    // switched with signals blocked.
    void refreshCloseButtons();
    QAbstractButton *createCloseButton();
};
