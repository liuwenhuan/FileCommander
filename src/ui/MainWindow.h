#pragma once

#include "OpenWithHandlers.h"
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QMainWindow>
#include <QElapsedTimer>
#include <QJsonObject>

#include <atomic>
#include <memory>
#include <QHash>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <functional>

#include <QLabel>

class QTimer;
class QTemporaryDir;

#include "FileListView.h"
#include "CommandRegistry.h"
#include "ShellShortcuts.h" // Destination, used by value in a slot signature
#include "ScratchDirs.h"
#include "StartupTrace.h"
#include "Settings.h"
#include "account/AccountClient.h" // AccountDeviceInfo (stored by value)
#include "account/PendingTransferStore.h"
#include "filesystem/ComputerCatalog.h" // ComputerEntry (passed by value)
#include "network/ConnectionStore.h" // SavedConnection (session reconnect)
#include "update/UpdateChecker.h" // UpdateInfo (stored by value)

#include <QVector>

class FilePanel;
class FileProvider;
class FunctionKeyBar;
class CommandBar;
class CommandOutputDialog;
class QuickView;
class ViewerWindow;
class OperationProgressDialog;
class OperationQueue;
class TransferProgressDialog;
class ThemeManager;
class QShortcut;
class QSplitter;
class QTreeView;
class QFileSystemModel;
class QAction;
class QMenu;
class TitleBar;
class RemovableDeviceMonitor;
class NetworkTreeRegistry;
class AccountClient;
class CloudClipboardController;
class DeviceAgent;
class FileShareServer;
class RelayTunnel;
class PendingSendQueue;
struct AccountSession;
class SmbHostBrowser;
class NotepadPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, qint64 startupElapsedMs = 0,
                        bool collectStartupPhases = false,
                        const QElapsedTimer *startupClock = nullptr);
    // Packaging-only smoke flow: browse `directory`, load each supplied local
    // preview fixture, then exit. It is intentionally not exposed through UI.
    void runPackageSmoke(const QString &directory);
    // Opens shell-activated local folders. The first two occupy the visible
    // panes; further folders become tabs in the left pane.
    void openFolders(const QStringList &folders);
    QJsonObject startupMetrics() const;
    // The "✳" menu's contents, with every command bound to `panel` rather than
    // to the active panel. Caller owns the returned menu. Public so a test can
    // exercise the commands without entering the popup's modal exec().
    QMenu *buildShortcutMenu(FilePanel *panel);
    // The "Open With" submenu for `path`, filled. Caller owns it. Public for
    // the same reason as buildShortcutMenu: the real one lives inside a modal
    // popup, and this is the only way a test can look at what it offers.
    QMenu *buildOpenWithMenu(const QString &path);
    void setActivePanel(FilePanel *panel);
    // Whether the silent update check should run now: automatic checking is on
    // and today's check has not been made yet.
    bool updateCheckIsDue() const;
    // How often the scheduled check wakes up to ask that question. Far shorter
    // than the once-a-day policy it enforces, because the policy is written in
    // calendar dates and the machine may have been asleep.
    static constexpr int kUpdateCheckTickMs = 60 * 60 * 1000; // 1 hour

signals:
    void startupReady();

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
    QuickView *ensureQuickView();
    TransferProgressDialog *ensureTransferProgressDialog();
    void scheduleMediaWarmupAfterFirstPaint();
    void scheduleFeatureBatchAfterFirstPaint();
    void resetStartupPanelLoad(FilePanel *panel, quint64 generation);
    void markStartupPanelLoaded(FilePanel *panel, quint64 generation);
    void scheduleStartupPanelInteraction(FilePanel *panel);
    qint64 elapsedSinceStartup() const;
    // F3 and F4 open the same window: preview and editor are two pages of one
    // QuickView and swap in place. `startEditing` picks which page it opens on.
    ViewerWindow *openViewerWindow(const QString &path, bool editable,
                                   bool startEditing = false,
                                   const QString &encodingIdentity = QString());
    // Resolves the entry under the cursor to a local path that can be EDITED --
    // a gvfs mount, never a downloaded copy -- and runs `then` on it. Explains
    // the refusal to the user and does not call `then` when there is none.
    void resolveEditableCurrent(std::function<void(const QString &)> then);

private slots:

    void viewCurrent();  // F3
    void editCurrent();  // F4
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
    // Toggles whether content (thumbnails, previews, video) follows the CRT
    // theme's phosphor hue. Only meaningful under Theme::Crt.
    void setPhosphorImages(bool on);
    void setPhosphorPreview(bool on);
    // Pushes the current theme + phosphor setting everywhere it is consumed
    // (stylesheet, icon tints, app icon, thumbnail memory cache, menu state).
    void applyTheme();
    // Redoes the artwork that was recoloured from the palette. Shared by
    // applyTheme() and the startup path, which applies the stylesheet directly.
    void refreshThemedArtwork();
    void setLanguage(const QString &language);
    void openMultiRenameDialog(); // Ctrl+M
    void openSyncDialog();
    void compareSelectedFiles();
    void compareDirectories();
    void openDirectoryHotlist(); // Ctrl+D
    void showProperties(); // F9
    void showShortcutMenu(FilePanel *panel, const QPoint &globalPos);
    // Directory-favorites menu from a tab-strip right-click. tabIndex is the
    // right-clicked tab so a chosen favorite navigates that tab (-1 = active tab).
    void showFavoritesMenu(const QPoint &globalPos, int tabIndex);
    // Re-establish the server connection saved on `panel`'s tab at `index` (its
    // reconnect descriptor), e.g. from the tab context menu's "重新连接".
    void reconnectSavedTab(FilePanel *panel, int index);
    void calculateSizes();
    void calculateChecksums(); // MD5 / CRC32 / SHA1 of the selected files
    void secureWipeSelected(); // overwrite local on-disk bytes, then delete
    void chooseGlobalFont();
    void syncOtherPanelToActive();

    // Total Commander compatibility actions (see the tc* bindShortcut block).
    void syncActiveToOtherPanel();   // Ctrl+I -- the inverse of the above
    void navigateToRoot();           // Ctrl+backslash
    void openCurrentEntryInNewTab(); // Ctrl+Up
    void showContextMenuForCurrent();// Shift+F10
    void copyPathToCommandLine();    // Ctrl+P
    void createNewTextFile();        // Shift+F4
    // Shared by Shift+F4 and the blank-area "New File" submenu: prompts for a
    // name, creates the file empty, and selects it. `openInEditor` is what
    // separates the keyboard route (which edits straight away) from the menu
    // items (which just drop the file in the listing, like "New Folder").
    void createNewFile(const QString &title, const QString &defaultName, bool openInEditor);
    void copyInSameDirectory();      // Shift+F5
    void swapPanels();
    void openTerminalHere();
    void openWithDefault();
    void openWith();
    // "Open With": the applications registered for the file's type, the rest
    // of what the system can launch, and the file dialog behind both.
    void fillOpenWithMenu(QMenu *menu, const QString &path);
    void runOpenWithHandler(const fc::OpenWithHandler &handler, const QString &path);
    void warmOpenWithIcons(const QStringList &programs,
                           const QVector<QPointer<QAction>> &actions);
    void chooseApplicationAndOpen();
    void toggleQuickView(); // Ctrl+Q
    void quickEditCurrent(); // Ctrl+E
    void updateQuickView();
    // Remote-preview download callbacks (invoked from the worker thread via a
    // queued invocation; reqId discards a stale download after the cursor moves).
    void onPreviewProgress(quint64 reqId, qint64 done, qint64 total);
    void onPreviewDone(quint64 reqId, const QString &tempPath, bool cancelled);
    // Remote-copy callbacks for "open with the associated application" (same
    // queued-invocation shape as the preview ones above, but one per in-flight
    // request: every open the user asked for must complete, none supersedes
    // another).
    void onRemoteFetchProgress(quint64 reqId, qint64 done, qint64 total);
    void onRemoteFetchDone(quint64 reqId, const QString &localPath, bool cancelled);
    // Archive worker callbacks (same queued-invocation shape). An extraction
    // reports one of these per level, because a nested level may need a password
    // and only the GUI thread can ask for one.
    void onArchiveJobProgress(quint64 reqId, const QString &entry, qint64 doneItems,
                              qint64 totalItems, qint64 doneBytes, qint64 totalBytes);
    void onExtractLevelDone(quint64 reqId, bool ok, int status, const QString &finalDir,
                            const QString &nested, const QString &error);
    void onCompressDone(quint64 reqId, bool ok, const QString &error);
    void undoLast(); // Ctrl+Z
    void runCommand(const QString &command, const QString &directory);
    void openExternalConnections(); // external-connect command (leading button default)
    // Computer view: the address row's computer button fills `panel` with the
    // drives, user folders, removable media, saved servers and discovered hosts;
    // activating one of those rows lands in openComputerEntry.
    void showComputerView(FilePanel *panel);
    void openComputerEntry(FilePanel *panel, const ComputerEntry &entry);
    // Re-lists every panel currently showing the computer view. Wired to the
    // device monitor and the host browser so a stick being plugged in or a host
    // being discovered shows up without the user having to leave and come back.
    void refreshComputerViews();
    void toggleNotepad();           // quick-notepad command (trailing button default)
    void showAboutDialog();         // View > About this program
    // Title-bar account entry: sign in / out, list the account's other
    // devices. The client behind it is created on first use and then kept, so
    // the session outlives the dialog.
    void showAccountDialog();

    // Creates m_accountClient if needed and restores the keyring session. Also
    // the place the title-bar name is wired up, so the entry follows the
    // session whether the dialog or startup brought the client into being.
    void ensureAccountClient();
    // Opens a tab browsing another device of the same account: asks the server
    // for a session, then points a plain CurlWebDavProvider at the peer's
    // FileShareServer. The peer needs no new client code and this needs no new
    // transfer code -- it is an ordinary network tab from here on.
    void openAccountDevice(const QString &deviceId, const QString &name);
    void openDeviceSession(const AccountSession &session, const QString &name);
    // Copies `sources` (local paths) into the peer's "Files I Received" folder,
    // over the same session and the same provider a device tab would use. The
    // peer needs no prompt: it is another device of the same account.
    void sendToDevice(const QString &deviceId, const QString &name, const QStringList &sources);

    // Starts or stops the serving half: on whenever an account is signed in,
    // because receiving is what "Send to Device" needs and it is always the
    // user's own devices at the other end. The folders the user chose to share
    // are added on top of the received-files folder only when sharing is on.
    void updateDeviceSharing();
    void checkForUpdatesNow();      // View > Check for Updates (manual)
    // The silent daily check. Runs at startup and on a timer thereafter; does
    // nothing unless updateCheckIsDue(). Never pops a dialog -- a release it
    // finds only lights the title-bar badge.
    void maybeRunScheduledUpdateCheck();
    void showUpdateDialog();        // opens the pending-update dialog

    void navigateBack();
    void navigateForward();
    void navigateUp();
    void refreshActivePanel();

    void handleFilesDropped(const QStringList &sources, const QString &destDir,
                             FileListView::DropActionKind kind, FileProvider *srcProvider);
    void copySelectionToClipboard();
    void cutSelectionToClipboard();
    void pasteFromClipboard();

private:
    // (Re)builds the Config/Interface/Actions menus and the title bar that hosts them.
    // Safe to call again on a language change (deletes the previous menus/bar).
    void buildTitleBarMenus();
    void syncConfigMenuState();
    void syncInterfaceMenuState();
    QAction *addCommandAction(QMenu *menu, const QString &id, const QString &label,
                              std::function<void()> handler = {});
    // The shortcut-menu entries, into a menu the caller already owns. The "✳"
    // popup and the menu bar's "Actions" entry both go through here, so the two
    // cannot drift apart.
    void fillShortcutMenu(QMenu *menu, FilePanel *panel);
    QString commandText(const QString &id, const QString &label) const;
    void applyInterfaceTypography();
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
    // The two square buttons flanking the F-key row (slot "leading"/"trailing").
    void runExtraKey(const QString &slot);
    void changeExtraKey(const QString &slot);
    void updateExtraKeyButtons();
    // Shared command picker for changeFunctionKey/changeExtraKey. Returns the
    // chosen command id, or an empty string if cancelled; currentId is preselected.
    QString pickCommandId(const QString &title, const QString &currentId);
    // Opens the "Connect to Server" dialog (optionally preselecting SMB, e.g.
    // from the network-neighborhood gear) and, on accept, connects the active
    // panel to the chosen server asynchronously.
    void openServerConnectDialog(bool preselectSmb);
    // Target panel/tab for a new server connection: always the LEFT panel, in a
    // fresh tab (per user preference) rather than replacing whatever the active
    // panel is showing. Focuses the left panel and returns it ready to connect.
    FilePanel *beginServerConnection();
    // Opens a saved bookmark / browses an SMB host's shares, each in a fresh tab
    // on the left panel. Shared by the connect fly-out and the computer view, so
    // the two cannot drift on credential retry or session bookkeeping.
    void openSavedConnection(const SavedConnection &conn);
    void browseSmbHost(const QString &hostName);
    // Everything the computer view lists, assembled from the catalog plus the
    // device monitor and host browser this window owns.
    QVector<ComputerEntry> computerEntries();
    // Fetches the drive icons on a worker and repaints `panel` when they land,
    // so the shell query never happens while painting. See the definition.
    void warmDriveIcons(FilePanel *panel);
    // Panels that must enter the computer view once the signal that
    // assembles its rows is connected -- a first launch's left pane, and any
    // tab the previous session left on the view. Both are decided while the
    // panels are being filled, which happens before that wiring exists.
    QVector<FilePanel *> m_startupComputerViewPanels;
    // Modal username/password prompt shown when a server needs credentials.
    // Returns true and fills *user/*pass on OK, false on cancel.
    bool promptCredentials(const QString &host, QString *user, QString *pass,
                           const QString &error = QString());
    FilePanel *otherPanel(FilePanel *panel) const;
    // The panel currently browsing `dir` (cleaned path match), or nullptr if
    // neither panel shows it (e.g. a drop onto a sub-folder that isn't open).
    FilePanel *panelShowingDir(const QString &dir) const;
    // The provider that owns `path`: a panel backed by a remote provider whose
    // current directory is (a prefix of) `path`, otherwise the local provider.
    // Lets drag-drop / paste pick the cross-provider transfer engine the same way
    // F5 copy/move does, instead of treating a remote path as a local file.
    std::shared_ptr<FileProvider> providerOwningPath(const QString &path) const;
    std::shared_ptr<FileProvider> providerPtrFor(FileProvider *provider) const;
    // Resolves a clipboard-tagged remote source (scheme + "user@host") back to a
    // still-live provider among the open panels, or null if that connection is
    // gone -- so a remote paste binds to the real source instead of the fragile
    // path guess, and refuses rather than silently reading a local file.
    std::shared_ptr<FileProvider> findLiveRemoteProvider(const QString &scheme,
                                                         const QString &displayName) const;
    // The paths files land at when `sources` are copied/moved/linked into
    // `destDir` (destDir joined with each source's base name).
    static QStringList destPathsFor(const QStringList &sources, const QString &destDir);
    // If `panel` is browsing a (read-only) archive, warn and return true so the
    // caller aborts the write op. Null panel → false.
    bool blockArchiveWrite(FilePanel *panel);
    // Refuses an operation that needs a directory to act in when the panel is
    // showing a flat search listing, which has none. Returns true when it said
    // so and the caller should stop.
    bool blockWithoutWorkingDirectory(FilePanel *panel, const QString &title);
    // Smart-extracts `archivePath` into `destDir`, prompting to recurse into a
    // single nested archive. Shared by the "Extract Here/To..." context actions.
    // `refreshPanel` is re-listed when the job ends, for the callers whose panel
    // path is not `destDir` (a network tab unpacking through a mount point).
    void smartExtractArchive(const QString &archivePath, const QString &destDir,
                             FilePanel *refreshPanel = nullptr);
    void showFileContextMenu(FilePanel *panel, const QPoint &viewPos);
    void showBlankContextMenu(FilePanel *panel, const QPoint &viewPos);
    // Fills a menu with "bookmark current" + separator + saved favorites,
    // shared by Ctrl+D (openDirectoryHotlist) and the tab-strip right-click menu.
    // tabIndex is the tab a chosen favorite should navigate (-1 = active tab).
    void populateFavoritesMenu(QMenu *menu, FilePanel *panel, int tabIndex = -1);
    void recordMoveUndo(const QStringList &sources, const QString &destDir);

    // Last reversible operation, for Ctrl+Z. Best-effort: rename, move, and trash delete.
    struct UndoRecord {
        enum Type { None, Rename, Move, Trash } type = None;
        QString fromPath;      // Rename: current path to move back
        QString toName;        // Rename: original name to restore
        QStringList movedPaths;// Move: current paths at the destination
        QString restoreDir;    // Move: original parent directory
        // Rename on a remote tab: the provider that owns fromPath, so undo goes
        // through the network backend instead of the local filesystem. Null =
        // local (undo via the plain rename path).
        std::shared_ptr<FileProvider> provider;
    };
    UndoRecord m_lastUndo;
    // Trash restore tokens have a different lifetime/owner from rename's remote
    // provider; keep them out of the mixed UndoRecord so queue completion only
    // copies value types across the async boundary.
    QStringList m_lastTrashPaths;
    QStringList m_lastTrashEntries;

    // Initialised, because anything the constructor triggers before it reaches
    // the panel construction can reach these -- and a raw uninitialised pointer
    // passes a null check.
    FilePanel *m_leftPanel = nullptr;
    FilePanel *m_rightPanel = nullptr;
    FilePanel *m_activePanel = nullptr;
    FunctionKeyBar *m_functionKeyBar;
    CommandBar *m_commandBar;
    CommandOutputDialog *m_commandOutput = nullptr; // lazily created on first command
    OperationQueue *m_queue;
    TransferProgressDialog *m_progressDialog = nullptr;
    QStringList m_operationErrors; // accumulated per-file errors for the running job
    bool m_operationAbortRequested = false;
    // Set while a delete is in flight so the queue-finished handler can remove
    // just those rows and select the next file, instead of a full rescan that
    // would snap the cursor back to the top of the list.
    FilePanel *m_pendingDeletePanel = nullptr;
    QStringList m_pendingDeletePaths;
    // Set while a move is in flight so the queue-finished handler removes the
    // vanished source rows in place (same as delete) instead of rescanning the
    // source directory.
    FilePanel *m_pendingMovePanel = nullptr;
    QStringList m_pendingMovePaths;
    // Set while a copy or move is in flight so the queue-finished handler
    // refreshes only the destination panel and selects the resulting file(s),
    // rather than full-refreshing both panels.
    FilePanel *m_pendingDestPanel = nullptr;
    QStringList m_pendingDestPaths;
    ThemeManager *m_themeManager;
    Settings m_settings;
    QSplitter *m_panelSplitter;
    QMenu *m_configMenu = nullptr;   // owned; rebuilt on language change
    QMenu *m_interfaceMenu = nullptr; // owned; rebuilt on language change
    QMenu *m_actionsMenu = nullptr;  // owned; rebuilt on language change

    // Frameless-chrome paint cache: the shadow + rounded frame rendered once at
    // a small canonical size and blitted 9-patch style at any window size.
    QPixmap m_frameCache;
    QColor m_frameCacheColor;     // window colour the cache was rendered with
    QTimer *m_maskTimer = nullptr; // coalesces rounded-corner mask updates
    QTimer *m_quickViewDebounce = nullptr; // delays the Ctrl+Q preview until the
                                           // cursor settles (skip big files while
                                           // arrow-scrolling past them)
    QTimer *m_mediaWarmTimer = nullptr;
    bool m_mediaWarmScheduled = false;
    bool m_mediaWarmComplete = false;
    // Phase timings, recorded in order under one name each. Was a member, a
    // recording line and an emit line per phase: the same name written three
    // times, in three places that had to be kept in agreement by hand.
    StartupTrace m_startupTrace;
    QElapsedTimer m_startupElapsed;
    QElapsedTimer m_startupClock;
    qint64 m_startupElapsedOffsetMs = 0;
    bool m_startupVisible = false;
    bool m_startupPanelLoaded[2] = {};
    bool m_startupPanelInteractionScheduled[2] = {};
    bool m_startupPanelInteractive[2] = {};
    quint64 m_startupPanelGeneration[2] = {};
    qint64 m_startupVisibleMs = -1;
    qint64 m_startupPanelsLoadedMs = -1;
    qint64 m_startupInteractiveMs = -1;
    bool m_collectStartupPhases = false;
    // Splits the panel-construction phase in two. The pair used to bracket
    // Typography::chromeFont() and the left panel together, and reported ~380 ms
    // against a right panel that costs 2 ms -- so the cost was one-time, and
    // there was no way to tell which of the two calls was paying it.

    QuickView *m_quickView = nullptr;
    FilePanel *m_quickViewPanel = nullptr; // panel replaced by the preview
    int m_quickViewIndex = -1;
    bool m_quickViewActive = false;
    // Network preview: a remote file must be downloaded to a real local temp file
    // before the viewers can open it. The download runs on a worker thread; the
    // request id discards a stale download when the cursor has moved on. The temp
    // dir is created lazily and auto-cleaned on exit.
    quint64 m_previewReqId = 0;
    bool m_previewRunning = false;                       // a remote fetch is in flight
    QString m_previewName;                               // current file's name (for messages)
    QString m_previewEncodingIdentity;                   // source before temp download
    std::shared_ptr<std::atomic<bool>> m_previewCancel;  // current download's cancel flag
    // The one file whose streamed preview failed, so retrying it downloads
    // instead of streaming again (which would fail the same way, forever).
    QString m_streamFailedEntry;
    // The three temporary directories, with the one real difference between
    // them (whether it outlives the program) recorded where it belongs.
    ScratchDirs m_scratch;
    QString ensurePreviewTempDir();
    void cancelPreviewDownload(); // Stop button: abort the current preview fetch

    // "Open with the associated application" for a file that has no local path.
    // A network tab's paths belong to the provider, not the filesystem, so the
    // file is streamed into a local temp copy first and the copy is handed to the
    // desktop. The copy is read-only and is NOT written back (see
    // openWithAssociatedApp), so an editor refuses to save onto it instead of
    // silently discarding the user's edits.
    struct RemoteFetch {
        QString name;                              // basename, for messages
        QString destDir;                           // per-request dir, removed if nothing lands
        qint64 total = 0;                          // expected bytes (0 = unknown)
        std::shared_ptr<std::atomic<bool>> cancel; // flipped by the Cancel button
        OperationProgressDialog *dialog = nullptr; // created only if slow (see 500ms timer)
        std::function<void(const QString &)> onReady; // what to do with the local copy
    };
    quint64 m_openReqId = 0;
    QHash<quint64, RemoteFetch> m_remoteFetches; // in-flight fetches, keyed by request id
    bool m_remoteCopyNoticeShown = false;        // the read-only notice is once per session
    QString ensureOpenTempDir();
    // Separate root for downloaded archives, which unlike the open-with copies
    // are only ever read by this process and are deleted as soon as the user
    // steps back out of the archive (see ArchiveProvider::setOwnsArchiveFile).
    QString ensureArchiveTempDir();

    // A running extraction or compression. ArchiveHandler walks the archive one
    // entry at a time through QFile, and on a gvfs-mounted share every one of
    // those reads crosses the wire, so the call runs on a worker thread and
    // reports back here. An extraction runs ONE level per worker run: recursing
    // into a nested archive has to come back to the GUI thread anyway, since an
    // encrypted level has to ask for a password.
    struct ArchiveJob {
        QString title;              // dialog title, also used in messages
        QString description;        // what the bar says about the level in flight
        QString source;             // archive being unpacked (extract) or written (compress)
        QString base;               // destination of the level in flight
        QString destDir;            // destination the user picked (level 0)
        QString finalDir;           // where the last finished level landed
        QString passphrase;         // carried into nested levels
        bool promptedForThisArchive = false;
        int depth = 0;
        QPointer<FilePanel> refreshPanel;          // panel to re-list when done
        std::shared_ptr<std::atomic<bool>> cancel; // flipped by the Cancel button
        OperationProgressDialog *dialog = nullptr; // created only if slow (500ms)
    };
    quint64 m_archiveJobId = 0;
    QHash<quint64, ArchiveJob> m_archiveJobs;
    void showArchiveJobProgressLater(quint64 reqId);
    void hideArchiveJobDialog(quint64 reqId); // for the prompts between levels
    void runExtractLevel(quint64 reqId);      // starts (or continues) an extraction
    void finishExtractJob(quint64 reqId, bool announce);
    void cancelArchiveJob(quint64 reqId); // Cancel button / shutdown
    // Whether the active panel's current entry is a directory, according to the
    // backend that listed it rather than to QFileInfo (which knows nothing about
    // a server's or an archive's paths).
    bool currentEntryIsDir() const;
    // Whether the current entry is an archive this machine can actually open --
    // a supported format AND on a local backend, since ArchiveHandler reads
    // through QFile. Gates both the menu item and the slots behind it.
    bool currentEntryIsExtractableArchive() const;
    void sendShortcutTo(fc::ShellShortcuts::Destination where);
    // Offers to make a non-executable AppImage executable. Returns whether the
    // file can be run afterwards -- false if the user declined or it failed, so
    // a caller can stop instead of launching something that cannot start.
    bool offerExecutableBit(const QString &path);
    // Asks for a REAL path on this machine for `path` as `panel` lists it, and
    // calls `then(realPath)` on the GUI thread -- with an empty string when
    // there isn't one, which is a normal answer every caller must handle.
    //
    // Local tabs answer instantly with the path itself. Network tabs go through
    // the gvfs mount (GvfsMounter::localPathFor), which is the only way a remote
    // file gets a name another program can open, and unlike a downloaded copy
    // that name is WRITABLE -- an editor saves back to the server through it.
    // Archive tabs always answer empty: an in-archive path names an entry, not
    // a file.
    //
    // Resolving can mount, and mounting a cold SFTP link took 2.4 s in testing,
    // so it runs on a worker thread under a busy cursor rather than freezing the
    // window. `then` is dropped if the panel goes away first.
    void resolveRealPath(FilePanel *panel, const QString &path,
                         std::function<void(const QString &)> then);
    // Opens `path` (as listed by `panel`) with the desktop's MIME-associated
    // application: through the gvfs mount when there is one, otherwise falling
    // back to fetching a read-only local copy.
    void openWithAssociatedApp(FilePanel *panel, const QString &path);
    // Launches the desktop's handler on an already-downloaded copy, warning if
    // nothing is associated and explaining once that the copy is read-only.
    void openLocalCopyWithDesktop(const QString &localPath, const QString &name);
    // Browses the archive at `path` off `panel`'s server in place as a virtual
    // folder (FilePanel::archiveDownloadRequested), through the gvfs mount when
    // there is one and by downloading a copy when there isn't.
    void browseRemoteArchive(FilePanel *panel, const QString &path);
    // The download half of the above, also used on its own when opening through
    // the mount was not possible.
    void browseRemoteArchiveByDownload(FilePanel *panel, const QString &path);
    // Streams `path` off `panel`'s provider into a local temp copy and calls
    // `onReady(localPath)` on the GUI thread once it is there. Reports its own
    // failures; `onReady` never runs on failure or cancellation. `destRoot`
    // overrides where the copy lands (default: the session-long open-with dir).
    void fetchRemoteCopy(FilePanel *panel, const QString &path,
                         std::function<void(const QString &)> onReady,
                         const QString &destRoot = QString());
    void cancelRemoteFetch(quint64 reqId); // Cancel button / shutdown
    // Gets `path` (as `panel` lists it) a name on this machine that opens the
    // same bytes, and hands it to `then`. Local tabs pass the path straight
    // through; anything else prefers the gvfs mount (no copying, works for a
    // multi-gigabyte file) and falls back to downloading a read-only copy.
    //
    // `then` does NOT run when neither works -- fetchRemoteCopy has already told
    // the user why -- so callers chaining two of these get no half-open dialog.
    void withLocalFile(FilePanel *panel, const QString &path,
                       std::function<void(const QString &)> then);

    // Every command, its label, its key and its handler. Was five parallel maps
    // here, kept in step by hand at each of the places that read them.
    CommandRegistry m_commands{this};
    QString m_fkeyCommands[6];  // command id per F3..F8 slot
    QString m_leadingCommand;   // command id for the square button before F3
    QString m_trailingCommand;  // command id for the square button after F8

    TitleBar *m_titleBar = nullptr; // self-drawn frameless title bar

    // Feature batch: external devices, quick notepad, online update.
    void setupFeatureBatch(); // constructor helper: wires the three subsystems below
    bool m_featureBatchScheduled = false;
    bool m_featureBatchStarted = false;
    RemovableDeviceMonitor *m_deviceMonitor = nullptr; // UDisks2 hot-plug watcher
    // Live network connections across both panels, so each panel's folder tree
    // can show a root per connection (and grey out the other panel's).
    NetworkTreeRegistry *m_connRegistry = nullptr;
    // Mount points of currently-mounted removable volumes. Diffed on every
    // devicesChanged() so a vanished mount (unmount or unplug) can auto-close
    // the tabs that were browsing it.
    QStringList m_removableMounts;
    SmbHostBrowser *m_smbBrowser = nullptr;            // SMB neighbourhood discovery
    // Created on first use by showAccountDialog() and kept afterwards: the
    // signed-in session (and, later, the device agent) has to outlive the
    // dialog that started it.
    // Asks the account server for a session against `deviceId` and calls `then`
    // with it. Answers asynchronously on a client that outlives the call, so
    // both handlers unhook themselves -- otherwise the next session opened
    // would also run this one's continuation.
    void withDeviceSession(const QString &deviceId, std::function<void(const AccountSession &)> then);

    // How to dial a peer device. connect() dials every candidate route -- the
    // peer's LAN addresses and the relay -- concurrently and returns the route
    // that won, or nullptr when none is reachable. The providers live inside
    // connect(); the relay tunnel rides along in the relay provider's deleter.
    struct DeviceLink {
        std::function<std::shared_ptr<FileProvider>(QString *)> connect;
    };
    DeviceLink deviceLink(const AccountSession &session);
    void downloadCloudClipboardImage(const AccountSession &session);

    AccountClient *m_accountClient = nullptr;
    CloudClipboardController *m_cloudClipboard = nullptr;
    PendingTransferStore m_pendingTransfers;
    // Last device list the server sent. A cache, because the "Send to Device"
    // submenu has to be populated the instant the menu opens; opening it also
    // asks for a fresh list, for the next time.
    QVector<AccountDeviceInfo> m_accountDevices;
    // The serving half, alive only while sharing is on: the agent keeps the
    // account server posted on this machine and hands over the tickets the
    // share server will accept.
    DeviceAgent *m_deviceAgent = nullptr;
    FileShareServer *m_shareServer = nullptr;
    // Sends queued while the target device was offline, drained when it next
    // shows as online. Session-only, see PendingSendQueue.
    PendingSendQueue *m_pendingSends = nullptr;
    // Relay sockets parked for peers that asked to connect, by session id.
    QHash<QString, RelayTunnel *> m_incomingTunnels;
    UpdateInfo m_pendingUpdate;                        // valid when m_hasUpdate
    bool m_hasUpdate = false;                          // an update is available
};
