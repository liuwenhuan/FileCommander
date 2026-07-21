#pragma once

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>

#include <QLabel>

#include "FileListView.h"
#include "Settings.h"

class FilePanel;
class FunctionKeyBar;
class CommandBar;
class QuickView;
class OperationQueue;
class OperationProgressDialog;
class ThemeManager;
class QShortcut;
class QSplitter;
class QTreeView;
class QFileSystemModel;
class QMenu;
class TitleBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    // Turns a click on the View-menu font-size label into a QInputDialog prompt.
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Keeps the title bar's maximize/restore glyph in sync with window state,
    // and drives frameless edge-resize hit testing.
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;
    // Paints the rounded window background + soft drop shadow into the
    // translucent margin (the frameless window has no WM decoration/shadow).
    void paintEvent(QPaintEvent *event) override;
    // Clips the central widget's bottom corners to match the rounded window
    // (the title bar rounds the top corners itself).
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void setActivePanel(FilePanel *panel);

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
    void compareDirectories();
    void openDirectoryHotlist(); // Ctrl+D
    void showProperties(); // F9
    void showShortcutMenu(const QPoint &globalPos);
    void showFavoritesMenu(const QPoint &globalPos); // "★" button in the address row
    void calculateSizes();
    void syncOtherPanelToActive();
    void swapPanels();
    void splitFile();
    void combineFiles();
    void openTerminalHere();
    void openWithDefault();
    void openWith();
    void toggleQuickView(); // Ctrl+Q
    void updateQuickView();
    void undoLast(); // Ctrl+Z
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
    // Registers an invokable command (id -> label + handler) without a
    // dedicated key -- used for the F-key slots and the "change function" list.
    void registerCommand(const QString &id, const QString &label, std::function<void()> handler);
    void runFunctionKey(int index);       // execute the command assigned to F(3+index)
    void changeFunctionKey(int index);    // pick a new command for that key
    void updateFunctionKeyLabels();
    FilePanel *otherPanel(FilePanel *panel) const;
    void showFileContextMenu(FilePanel *panel, const QPoint &viewPos);
    void showBlankContextMenu(FilePanel *panel, const QPoint &viewPos);
    // Fills a menu with "bookmark current" + separator + saved favorites,
    // shared by Ctrl+D (openDirectoryHotlist) and the "★" address-row button.
    void populateFavoritesMenu(QMenu *menu, FilePanel *panel);
    void recordMoveUndo(const QStringList &sources, const QString &destDir);

    // Last reversible operation, for Ctrl+Z. Best-effort: rename and move only.
    struct UndoRecord {
        enum Type { None, Rename, Move } type = None;
        QString fromPath;      // Rename: current path to move back
        QString toName;        // Rename: original name to restore
        QStringList movedPaths;// Move: current paths at the destination
        QString restoreDir;    // Move: original parent directory
    };
    UndoRecord m_lastUndo;

    FilePanel *m_leftPanel;
    FilePanel *m_rightPanel;
    FilePanel *m_activePanel = nullptr;
    FunctionKeyBar *m_functionKeyBar;
    CommandBar *m_commandBar;
    OperationQueue *m_queue;
    OperationProgressDialog *m_progressDialog;
    QStringList m_operationErrors; // accumulated per-file errors for the running job
    ThemeManager *m_themeManager;
    Settings m_settings;
    QSplitter *m_outerSplitter;
    QSplitter *m_panelSplitter;
    QTreeView *m_folderTree;
    QFileSystemModel *m_folderTreeModel;

    QuickView *m_quickView = nullptr;
    FilePanel *m_quickViewPanel = nullptr; // panel replaced by the preview
    int m_quickViewIndex = -1;
    bool m_quickViewActive = false;

    QMap<QString, QShortcut *> m_shortcuts;
    QMap<QString, QKeySequence> m_shortcutDefaults;
    QMap<QString, std::function<void()>> m_shortcutHandlers; // id -> action (all commands)
    QMap<QString, QString> m_commandLabels;                  // id -> label (all commands)
    QList<QPair<QString, QString>> m_shortcutOrder; // id, label (keyed shortcuts only)
    QString m_fkeyCommands[6];  // command id per F3..F8 slot

    // File-list font-size control living in the View menu.
    QLabel *m_fontSizeLabel = nullptr;
    std::function<void(int)> m_applyListFontSize;

    TitleBar *m_titleBar = nullptr; // self-drawn frameless title bar
};
