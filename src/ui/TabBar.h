#pragma once

#include <QTabBar>

class QAbstractButton;
class QPropertyAnimation;
class QResizeEvent;

// Visual tab strip for one FilePanel. Thin wrapper around QTabBar adding
// the right-click directory-favorites menu (bookmark toggle + jump list,
// synced with Ctrl+D). Tab state itself lives in TabManager, not here.
class TabBar : public QTabBar {
    Q_OBJECT
    Q_PROPERTY(qreal activationProgress READ activationProgress WRITE setActivationProgress)

public:
    explicit TabBar(QWidget *parent = nullptr);
    qreal activationProgress() const { return m_activationProgress; }

    // FilePanel owns the visible left overflow control. Delegate its click to
    // Qt's hidden native helper so scrolling behavior stays platform-correct.
    void scrollLeft();

signals:
    void closeTabRequested(int index);
    void overflowScrollButtonsVisibleChanged(bool visible);
    // Right-click on the tab strip opens the directory-favorites menu at this
    // point; the panel/window builds and shows it (toggle + saved favorites).
    // tabIndex is the tab under the cursor (-1 if the click missed the tabs).
    void favoritesMenuRequested(const QPoint &globalPos, int tabIndex);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // Repaints Qt's native overflow controls with compact theme-aware
    // chevrons; the hidden left helper is still used for scrolling.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setActivationProgress(qreal progress);
    void animateActivation();
    // Keeps the "×" buttons in sync: they all use the tab label colour (palette
    // WindowText) for legibility, and a lone tab shows none at all. Driven from
    // paintEvent so it always sees the correct tab count, even when tabs are
    // switched with signals blocked.
    void refreshCloseButtons();
    void syncScrollButtons();
    QAbstractButton *createCloseButton();
    bool m_scrollButtonsVisible = false;
    qreal m_activationProgress = 1.0;
    QPropertyAnimation *m_activationAnimation = nullptr;
};
