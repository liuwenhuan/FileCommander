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

    // Runs the current-tab visual transition after a caller intentionally
    // suppressed currentChanged while installing consistent tab state.
    void animateCurrentTabActivation();

signals:
    void closeTabRequested(int index);
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

private:
    void setActivationProgress(qreal progress);
    void animateActivation();
    // Keeps the "×" buttons in sync: they all use the tab label colour (palette
    // WindowText) for legibility, and a lone tab shows none at all. Driven from
    // paintEvent so it always sees the correct tab count, even when tabs are
    // switched with signals blocked.
    void refreshCloseButtons();
    QAbstractButton *createCloseButton();
    qreal m_activationProgress = 1.0;
    QPropertyAnimation *m_activationAnimation = nullptr;
};
