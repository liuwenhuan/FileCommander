#pragma once

#include <QWidget>

#include "FileSystemModel.h"
#include "TabManager.h"

class QLineEdit;
class FileListView;
class TabBar;

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

    void selectAll();
    void deselectAll();
    void invertSelection();

    void newTab();
    void closeCurrentTab();
    void nextTab();
    void prevTab();

    FileSystemModel *model() const { return m_model; }
    FileListView *view() const { return m_view; }

signals:
    void pathChanged(const QString &path);
    void panelActivated(FilePanel *panel);
    // A file (not a directory) was double-clicked / Enter-pressed.
    void openRequested(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onActivated(const QModelIndex &index);
    void onAddressBarEntered();
    void onTabBarCurrentChanged(int index);

private:
    void pushHistory(const QString &fromPath);
    QString tabLabelFor(const QSharedPointer<TabState> &tab) const;
    void syncTabBarFromManager();
    void closeTabAt(int index);
    void saveCurrentTabState();
    void loadTabState(int index);
    void updateActiveTabLabel();

    QLineEdit *m_addressBar;
    FileListView *m_view;
    FileSystemModel *m_model;
    QStringList m_backHistory;
    QStringList m_forwardHistory;

    TabManager *m_tabManager;
    TabBar *m_tabBar;
    QStringList m_pendingSelection;
};
