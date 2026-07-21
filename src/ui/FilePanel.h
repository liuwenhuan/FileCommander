#pragma once

#include <QPair>
#include <QVector>
#include <QWidget>

#include "FileSystemModel.h"
#include "TabManager.h"

class BreadcrumbBar;
class FileListView;
class StatusBarWidget;
class TabBar;
class QLineEdit;
class QToolButton;

// One side of the dual-pane layout: address bar + file list + per-panel
// back/forward history. Two of these live in MainWindow (left/right).
class FilePanel : public QWidget {
    Q_OBJECT

public:
    explicit FilePanel(QWidget *parent = nullptr);

    QString currentPath() const { return m_model->rootPath(); }
    void navigateTo(const QString &path);
    void navigateUp();
    void goBack();
    void goForward();
    void refresh();

    // Path under the keyboard cursor (not necessarily selected) -- used by
    // F3/F4 to know which single file to open.
    QString currentEntryPath() const;
    QStringList selectedPaths() const;

    // Sets the point size of the file-list font and rescales the row height /
    // header height to match. Point size is clamped to 7..24.
    void setListFontSize(int pt);

    void selectAll();
    void deselectAll();
    void invertSelection();
    void toggleHiddenFiles();

    // Queues a path to be selected, focused, and scrolled into view once the
    // pending directory reload finishes -- used after an inline rename so the
    // listing doesn't snap back to the top.
    void selectPathAfterReload(const QString &path);

    // Marks this panel as the active one (of the two). Softens the inactive
    // panel's selection colour so the active panel's cursor row stands out.
    void setActive(bool active);

    // Prompts for a wildcard mask (e.g. *.txt) and adds matching files to the
    // selection (select=true) or removes them (select=false).
    void selectByPattern(bool select);

    // Recursively computes the size of each selected directory (or the one
    // under the cursor) off the UI thread and shows it in the Size column.
    void calculateDirSizes();

    void newTab();
    void closeCurrentTab();
    void nextTab();
    void prevTab();

    // Reveals the quick-filter box and gives it focus. Esc (handled in the
    // event filter) hides it and restores the full listing.
    void showQuickFilter();
    void hideQuickFilter();

    // Session persistence: plain Qt types (not TabState) so MainWindow can
    // hand these to core/config's SessionManager without ui depending on
    // it, or core depending on ui.
    QVector<QPair<QString, QStringList>> tabSnapshot();
    int activeTabIndex() const { return m_tabManager->activeIndex(); }
    void restoreTabs(const QVector<QPair<QString, QStringList>> &tabs, int activeIndex);

    FileSystemModel *model() const { return m_model; }
    FileListView *view() const { return m_view; }

    // Re-applies translated text (the status-bar object/selection counts) after a
    // live UI-language change.
    void retranslate() { updateStatus(); }

signals:
    void pathChanged(const QString &path);
    void panelActivated(FilePanel *panel);
    // A file (not a directory) was double-clicked / Enter-pressed.
    void openRequested(const QString &path);
    // Tab pressed in the list: caller should activate the other panel.
    void switchPanelRequested();
    // The "*" button was clicked: caller should pop a shortcut menu at pos.
    void shortcutMenuRequested(const QPoint &globalPos);
    // The "★" button was clicked: caller should pop the favorites menu at pos.
    void favoritesMenuRequested(const QPoint &globalPos);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onActivated(const QModelIndex &index);
    void onAddressBarEntered(const QString &path);
    void onTabBarCurrentChanged(int index);

private:
    void pushHistory(const QString &fromPath);
    void updateStatus();
    void updateNavButtons();
    QString tabLabelFor(const QSharedPointer<TabState> &tab) const;
    void syncTabBarFromManager();
    void closeTabAt(int index);
    void saveCurrentTabState();
    void loadTabState(int index);
    void updateActiveTabLabel();

    BreadcrumbBar *m_addressBar;
    QToolButton *m_backButton;
    QToolButton *m_forwardButton;
    QToolButton *m_favButton;  // "★" directory favorites, left of the address bar
    QToolButton *m_starButton;
    QToolButton *m_addTabButton; // "+" at the right end of the tab strip
    QLineEdit *m_filterBar;
    FileListView *m_view;
    StatusBarWidget *m_statusBar;
    FileSystemModel *m_model;
    QStringList m_backHistory;
    QStringList m_forwardHistory;

    TabManager *m_tabManager;
    TabBar *m_tabBar;
    QStringList m_pendingSelection;
};
