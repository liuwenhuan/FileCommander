#pragma once

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>

#include "FileListView.h"
#include "Settings.h"

class FilePanel;
class FunctionKeyBar;
class CommandBar;
class StatusBarWidget;
class OperationQueue;
class OperationProgressDialog;
class ThemeManager;
class QShortcut;
class QSplitter;
class QTreeView;
class QFileSystemModel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void setActivePanel(FilePanel *panel);
    void updateStatusBar();

    void viewCurrent();  // F3
    void editCurrent();  // F4 (stub for now, Phase 2 adds TextEditor)
    void copySelected();  // F5
    void moveSelected();  // F6
    void makeDirectory();// F7
    void deleteSelected(bool permanent = false); // F8 / Shift+F8
    void renameCurrent(); // F2
    void compressSelected(); // Alt+F5
    void openSearch(); // Ctrl+F
    void openShortcutsDialog();
    void setTheme(Settings::Theme theme);
    void setLanguage(const QString &language);
    void openMultiRenameDialog(); // Ctrl+M
    void openSyncDialog();
    void compareSelectedFiles();
    void openDirectoryHotlist(); // Ctrl+D
    void showProperties(); // F9
    void runCommand(const QString &command, const QString &directory);
    void toggleFolderTree();

    void navigateBack();
    void navigateForward();
    void navigateUp();
    void refreshActivePanel();

    void handleFilesDropped(const QStringList &sources, const QString &destDir,
                             FileListView::DropActionKind kind);
    void copySelectionToClipboard();
    void cutSelectionToClipboard();
    void pasteFromClipboard();

private:
    void setupMenuAndToolbar();
    void setupShortcuts();
    void bindShortcut(const QString &id, const QString &label, const QKeySequence &defaultSeq,
                       std::function<void()> handler);
    FilePanel *otherPanel(FilePanel *panel) const;
    void showFileContextMenu(FilePanel *panel, const QPoint &viewPos);
    void showBlankContextMenu(FilePanel *panel, const QPoint &viewPos);

    FilePanel *m_leftPanel;
    FilePanel *m_rightPanel;
    FilePanel *m_activePanel = nullptr;
    FunctionKeyBar *m_functionKeyBar;
    CommandBar *m_commandBar;
    StatusBarWidget *m_statusBarWidget;
    OperationQueue *m_queue;
    OperationProgressDialog *m_progressDialog;
    QStringList m_operationErrors; // accumulated per-file errors for the running job
    ThemeManager *m_themeManager;
    Settings m_settings;
    QSplitter *m_outerSplitter;
    QTreeView *m_folderTree;
    QFileSystemModel *m_folderTreeModel;

    QMap<QString, QShortcut *> m_shortcuts;
    QMap<QString, QKeySequence> m_shortcutDefaults;
    QList<QPair<QString, QString>> m_shortcutOrder; // id, human-readable label
};
