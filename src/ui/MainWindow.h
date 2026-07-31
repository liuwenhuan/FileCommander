#pragma once

#include <QKeySequence>
#include <QList>
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
#include "Settings.h"
#include "network/ConnectionStore.h" // SavedConnection (session reconnect)
#include "update/UpdateChecker.h" // UpdateInfo (stored by value)

#include <QVector>

class FilePanel;
class FileProvider;
class FunctionKeyBar;
class CommandBar;
class CommandOutputDialog;
class QuickView;
class OperationProgressDialog;
class OperationQueue;
class TransferProgressDialog;
class ThemeManager;
class QShortcut;
class QSplitter;
class QTreeView;
class QFileSystemModel;
class QMenu;
class TitleBar;
class RemovableDeviceMonitor;
class NetworkTreeRegistry;
class SmbHostBrowser;
class NotepadPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, qint64 startupElapsedMs = 0);
    // Packaging-only smoke flow: browse `directory`, load each supplied local
    // preview fixture, then exit. It is intentionally not exposed through UI.
    void runPackageSmoke(const QString &directory);
    // Opens shell-activated local folders. The first two occupy the visible
    // panes; further folders become tabs in the left pane.
    void openFolders(const QStringList &folders);
    QJsonObject startupMetrics() const;

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
    void markStartupPanelLoaded(FilePanel *panel);
    qint64 elapsedSinceStartup() const;

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
    // Toggles whether content (thumbnails, previews, video) follows the CRT
    // theme's phosphor hue. Only meaningful under Theme::Crt.
    void setPhosphorImages(bool on);
    // Pushes the current theme + phosphor setting everywhere it is consumed
    // (stylesheet, icon tints, app icon, thumbnail memory cache, menu state).
    void applyTheme();
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
    void swapPanels();
    void openTerminalHere();
    void openWithDefault();
    void openWith();
    void toggleQuickView(); // Ctrl+Q
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
    void undoLast(); // Ctrl+Z
    void runCommand(const QString &command, const QString &directory);
    void openExternalConnections(); // external-connect command (leading button default)
    void toggleNotepad();           // quick-notepad command (trailing button default)
    void showAboutDialog();         // View > About this program
    void checkForUpdatesNow();      // View > Check for Updates (manual)
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
    // (Re)builds the Tools/Config/Interface menus and the title bar that hosts them.
    // Safe to call again on a language change (deletes the previous menus/bar).
    void buildTitleBarMenus();
    void syncConfigMenuState();
    void syncInterfaceMenuState();
    QAction *addCommandAction(QMenu *menu, const QString &id, const QString &label,
                              std::function<void()> handler = {});
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
    // Smart-extracts `archivePath` into `destDir`, prompting to recurse into a
    // single nested archive. Shared by the "Extract Here/To..." context actions.
    void smartExtractArchive(const QString &archivePath, const QString &destDir);
    void showFileContextMenu(FilePanel *panel, const QPoint &viewPos);
    void showBlankContextMenu(FilePanel *panel, const QPoint &viewPos);
    // Fills a menu with "bookmark current" + separator + saved favorites,
    // shared by Ctrl+D (openDirectoryHotlist) and the tab-strip right-click menu.
    // tabIndex is the tab a chosen favorite should navigate (-1 = active tab).
    void populateFavoritesMenu(QMenu *menu, FilePanel *panel, int tabIndex = -1);
    void recordMoveUndo(const QStringList &sources, const QString &destDir);

    // Last reversible operation, for Ctrl+Z. Best-effort: rename and move only.
    struct UndoRecord {
        enum Type { None, Rename, Move } type = None;
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

    FilePanel *m_leftPanel;
    FilePanel *m_rightPanel;
    FilePanel *m_activePanel = nullptr;
    FunctionKeyBar *m_functionKeyBar;
    CommandBar *m_commandBar;
    CommandOutputDialog *m_commandOutput = nullptr; // lazily created on first command
    OperationQueue *m_queue;
    TransferProgressDialog *m_progressDialog = nullptr;
    QStringList m_operationErrors; // accumulated per-file errors for the running job
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
    QMenu *m_toolsMenu = nullptr;    // owned; rebuilt on language change
    QMenu *m_configMenu = nullptr;   // owned; rebuilt on language change
    QMenu *m_interfaceMenu = nullptr; // owned; rebuilt on language change
    QAction *m_phosphorImagesAction = nullptr; // enabled only under the CRT theme
    bool m_shortcutsBuilt = false;   // one-shot guard for QShortcut creation

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
    QElapsedTimer m_startupElapsed;
    qint64 m_startupElapsedOffsetMs = 0;
    bool m_startupVisible = false;
    bool m_startupPanelLoaded[2] = {};
    bool m_startupPanelInteractive[2] = {};
    qint64 m_startupVisibleMs = -1;
    qint64 m_startupPanelsLoadedMs = -1;
    qint64 m_startupInteractiveMs = -1;

    QuickView *m_quickView = nullptr;
    FilePanel *m_quickViewPanel = nullptr; // panel replaced by the preview
    int m_quickViewIndex = -1;
    bool m_quickViewActive = false;
    // Network preview: a remote file must be downloaded to a real local temp file
    // before the viewers can open it. The download runs on a worker thread; the
    // request id discards a stale download when the cursor has moved on. The temp
    // dir is created lazily and auto-cleaned on exit.
    QTemporaryDir *m_previewTempDir = nullptr;
    quint64 m_previewReqId = 0;
    bool m_previewRunning = false;                       // a remote fetch is in flight
    QString m_previewName;                               // current file's name (for messages)
    std::shared_ptr<std::atomic<bool>> m_previewCancel;  // current download's cancel flag
    // The one file whose streamed preview failed, so retrying it downloads
    // instead of streaming again (which would fail the same way, forever).
    QString m_streamFailedEntry;
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
    QTemporaryDir *m_openTempDir = nullptr; // holds every fetched copy for the session
    quint64 m_openReqId = 0;
    QHash<quint64, RemoteFetch> m_remoteFetches; // in-flight fetches, keyed by request id
    bool m_remoteCopyNoticeShown = false;        // the read-only notice is once per session
    QString ensureOpenTempDir();
    // Separate root for downloaded archives, which unlike the open-with copies
    // are only ever read by this process and are deleted as soon as the user
    // steps back out of the archive (see ArchiveProvider::setOwnsArchiveFile).
    QTemporaryDir *m_archiveTempDir = nullptr;
    QString ensureArchiveTempDir();
    // Whether the active panel's current entry is a directory, according to the
    // backend that listed it rather than to QFileInfo (which knows nothing about
    // a server's or an archive's paths).
    bool currentEntryIsDir() const;
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

    QMap<QString, QShortcut *> m_shortcuts;
    QMap<QString, QKeySequence> m_shortcutDefaults;
    QMap<QString, std::function<void()>> m_shortcutHandlers; // id -> action (all commands)
    QMap<QString, QString> m_commandLabels;                  // id -> label (all commands)
    QList<QPair<QString, QString>> m_shortcutOrder; // id, label (keyed shortcuts only)
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
    UpdateInfo m_pendingUpdate;                        // valid when m_hasUpdate
    bool m_hasUpdate = false;                          // an update is available
};
