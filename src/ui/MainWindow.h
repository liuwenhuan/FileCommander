#pragma once

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <functional>

#include <QLabel>

class QTimer;

#include "FileListView.h"
#include "Settings.h"

class FilePanel;
class FunctionKeyBar;
class CommandBar;
class CommandOutputDialog;
class QuickView;
class OperationQueue;
class TransferProgressDialog;
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
    // Sets the initial _NET_WM_OPAQUE_REGION once the native window exists, and
    // syncs the shadow margin + corner mask to the actual maximized state (which
    // may be unknown during construction, so a window restored maximized would
    // otherwise keep the shadow-margin ring).
    void showEvent(QShowEvent *event) override;

private:
    // Rebuilds m_frameCache (the pre-rendered shadow + rounded frame) if the
    // theme colour changed; paintEvent blits it 9-patch style instead of
    // rasterizing 17 anti-aliased rounded rects on every repaint.
    void ensureFrameCache();
    // Applies the rounded-bottom-corner mask to the central widget. Invoked
    // via m_maskTimer so a drag-resize coalesces to one XShape update instead
    // of one per pixel.
    void applyRoundedMask();
    // Publishes the opaque interior (window minus the translucent shadow margin
    // and rounded corners) as _NET_WM_OPAQUE_REGION so the compositor skips
    // alpha-blending it. No-op off X11. Coordinates are in device pixels.
    void updateOpaqueRegion();

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
    void extractArchiveHere();  // Bandizip-style smart extract into the current dir
    void extractArchiveToDir(); // ... into a chosen directory
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
    void calculateChecksums(); // MD5 / CRC32 / SHA1 of the selected files
    void secureWipeSelected(); // overwrite on-disk bytes (HDD 1x / SSD 3x DoD) then delete
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
    // (Re)builds the Commands/View menus and the title bar that hosts them.
    // Safe to call again on a language change (deletes the previous menus/bar).
    void buildTitleBarMenus();
    // Re-applies every translatable string in the persistent UI after a live
    // language switch (menus, title bar, function keys, column headers, ...).
    void retranslateUi();
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
    // If `panel` is browsing a (read-only) archive, warn and return true so the
    // caller aborts the write op. Null panel → false.
    bool blockArchiveWrite(FilePanel *panel);
    // Smart-extracts `archivePath` into `destDir`, prompting to recurse into a
    // single nested archive. Shared by the "Extract Here/To..." context actions.
    void smartExtractArchive(const QString &archivePath, const QString &destDir);
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
    CommandOutputDialog *m_commandOutput = nullptr; // lazily created on first command
    OperationQueue *m_queue;
    TransferProgressDialog *m_progressDialog;
    QStringList m_operationErrors; // accumulated per-file errors for the running job
    ThemeManager *m_themeManager;
    Settings m_settings;
    QSplitter *m_panelSplitter;
    QMenu *m_toolsMenu = nullptr;    // owned; rebuilt on language change
    QMenu *m_configMenu = nullptr;   // owned; rebuilt on language change
    QMenu *m_viewMenu = nullptr;     // owned; rebuilt on language change
    bool m_shortcutsBuilt = false;   // one-shot guard for QShortcut creation

    // Frameless-chrome paint cache: the shadow + rounded frame rendered once at
    // a small canonical size and blitted 9-patch style at any window size.
    QPixmap m_frameCache;
    QColor m_frameCacheColor;     // window colour the cache was rendered with
    QTimer *m_maskTimer = nullptr; // coalesces rounded-corner mask updates
    QTimer *m_quickViewDebounce = nullptr; // delays the Ctrl+Q preview until the
                                           // cursor settles (skip big files while
                                           // arrow-scrolling past them)

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

    TitleBar *m_titleBar = nullptr; // self-drawn frameless title bar
};
