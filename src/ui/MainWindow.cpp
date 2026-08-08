#include "MainWindow.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QListView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QKeyEvent>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <QLabel>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QWidgetAction>
#include <QTreeWidget>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>

#include "FramelessDialog.h"
#include "ThemedDialogs.h"
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QRegion>
#include <QShowEvent>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QFontDialog>
#include <QFontDatabase>
#include <QIntValidator>
#include <QTimer>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QHeaderView>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

#if FILECOMMANDER_HAS_LINUX_INTEGRATION
#include <QX11Info>     // Qt5::X11Extras: xcb connection for the opaque-region hint
#include <xcb/xcb.h>
#include <cstdlib>      // free() for xcb replies
#endif

#include "ArchiveHandler.h"
#include "CommandBar.h"
#include "ShellCommand.h"
#include "OpenWithHandlers.h"
#include "TerminalLauncher.h"
#include "TranslationManager.h"
#include "CompressDialog.h"
#include "FilePanel.h"
#include "filesystem/IconCache.h"
#include "IconFileView.h"
#if FILECOMMANDER_MEDIA_BACKEND_MPV
#include "MpvStreamSource.h"
#endif
#include "QuickView.h"
#include "ThumbnailCache.h"
#include "ViewerWindow.h"
#include "FileListView.h"
#include "FileSystemModel.h"
#include "ExternalPaths.h"
#include "filesystem/ComputerCatalog.h"
#include "LocalFileProvider.h"
#include "FunctionKeyBar.h"
#include "ImageViewer.h"
#include "OperationQueue.h"
#include "diagnostics/RuntimeCounters.h"
#include "SearchDialog.h"
#include "SessionManager.h"
#include "TitleBar.h"
#include "dialogs/ChecksumDialog.h"
#include "dialogs/CommandOutputDialog.h"
#include "dialogs/ConnectDialog.h"
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
#include "dialogs/SecureWipeConfirmationDialog.h"
#include "dialogs/SecureWipeDialog.h"
#endif
#include "Settings.h"
#include "ThemeManager.h"
#include "TextEditor.h"
#include "Typography.h"
#include "dialogs/CompareDialog.h"
#include "dialogs/DeleteConfirmDialog.h"
#include "dialogs/MultiRenameDialog.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/TransferProgressDialog.h"
#include "dialogs/OverwriteConfirmDialog.h"
#include "dialogs/OperationErrorDialog.h"
#include "privilege/PrivilegeBroker.h"
#include "dialogs/PropertiesDialog.h"
#include "dialogs/ShortcutsDialog.h"
#include "dialogs/SyncDialog.h"

// Feature batch: external-connection picker, quick notepad, online update.
#include "NotepadPanel.h"
#include "dialogs/AboutDialog.h"
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
#include "dialogs/ExternalConnectDialog.h"
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
#include "dialogs/UpdateDialog.h"
#endif
#include "tree/NetworkTreeRegistry.h"
#if FILECOMMANDER_HAS_NETWORK
#include "network/CurlFtpProvider.h"
#include "network/CurlWebDavProvider.h"
#include "network/ConnectionStore.h"
#include "network/SftpProvider.h"
#if defined(Q_OS_WIN)
#include "network/WindowsSmbProvider.h"
#endif
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
#include "devices/RemovableDeviceMonitor.h"
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
#include "network/GvfsMounter.h"
#include "network/SmbProvider.h"
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
#include "network/SmbHostBrowser.h"
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
#include "update/UpdateChecker.h"
#endif

#include <QApplication>
#include <QDate>
#include <memory>

namespace {

// Repeated Windows measurements recorded 19-25 ms outliers. Task 3 requires
// pure first-use initialization when any supported-machine warm exceeds 16 ms.
constexpr bool kAutomaticMediaWarmEnabled = false;
constexpr int kStartupFeatureDelayMs = 5000;
std::shared_ptr<FileProvider> localProviderPtr() {
    return std::shared_ptr<FileProvider>(LocalFileProvider::instance(), [](FileProvider *) {});
}

// A splitter whose handle paints its own grey line across the full panel
// height (tabs, breadcrumb, list, status bar). The deepin (DTK) style ignores
// the handle's palette/autoFillBackground, so we paint it ourselves.
class PaintedHandle : public QSplitterHandle {
public:
    PaintedHandle(Qt::Orientation o, QSplitter *parent) : QSplitterHandle(o, parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        // Mid, not a literal grey: the handle is a divider between two panels
        // and should sit at the theme's own idea of one.
        p.fillRect(rect(), palette().color(QPalette::Mid));
    }
};

class PanelSplitter : public QSplitter {
public:
    explicit PanelSplitter(QWidget *parent) : QSplitter(Qt::Horizontal, parent) {}

protected:
    QSplitterHandle *createHandle() override { return new PaintedHandle(orientation(), this); }
};

// Total bytes of `paths`, taken from the listing `model` already holds instead
// of from QFileInfo. The two confirmation prompts that use this (delete, secure
// wipe) also run on network tabs, where the paths are the server's: a QFileInfo
// over one of them describes a same-named LOCAL file, or nothing at all, so the
// prompt used to offer "0 bytes" for a file that was 1.2 MB on the server.
// Directories contribute 0, exactly as they did before -- their recursive size
// isn't known here without a walk.
qint64 sumSizes(const FileSystemModel *model, const QStringList &paths) {
    if (!model)
        return 0;
    QHash<QString, qint64> sizeByPath;
    const int rows = model->rowCount();
    for (int r = 0; r < rows; ++r) {
        const FileInfo info = model->fileInfoAt(r);
        if (!info.isDir())
            sizeByPath.insert(info.path(), info.size());
    }
    qint64 total = 0;
    for (const QString &p : paths)
        total += sizeByPath.value(p, 0);
    return total;
}

// Private clipboard format tagging remote (SFTP/FTP/WebDAV/SMB) sources with the
// connection they came from, so paste routes them through the right provider
// instead of the fragile path-ancestor guess -- and, crucially, refuses rather
// than silently reading a same-named LOCAL file when the source connection is
// gone. Layout: line0 "cut"|"copy", line1 scheme, line2 displayName (user@host),
// then one remote path per line.
constexpr char kRemoteClipboardMime[] = "application/x-filecommander-remote-copy";

// Builds clipboard data with both the plain text/uri-list format (read by
// virtually everything) and the GNOME x-special/gnome-copied-files
// convention (read by Nautilus/Dolphin/PCManFM) so cut vs. copy survives
// round-tripping through those file managers, not just within FileCommander. When the
// source is a remote provider, also attaches kRemoteClipboardMime so an in-app
// paste can recover the true source provider (see pasteFromClipboard).
//
// The two public formats are built by fc::externalUrlsFor(), NOT from `paths`
// directly: on a network or archive tab those paths belong to the server or to
// the archive, and a file:// URL over one of them makes the receiving program
// open a same-named LOCAL file. fc::kInternalPathsMime carries the real paths
// for our own paste, which is why the public list can afford to be empty.
QMimeData *buildFileClipboardData(const QStringList &paths, bool cut,
                                  FileProvider *srcProvider = nullptr) {
    auto *mime = new QMimeData;
    fc::setPathPayload(mime, srcProvider, paths, cut);

    const QList<QUrl> urls = mime->urls();
    if (!urls.isEmpty()) {
        QByteArray gnomeFormat = cut ? "cut\n" : "copy\n";
        for (const QUrl &url : urls)
            gnomeFormat += url.toString().toUtf8() + "\n";
        mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeFormat);
    }

    // A remote source: tag it with scheme + displayName so paste binds it back to
    // the live connection rather than treating the path as a local file.
    if (srcProvider && !srcProvider->scheme().isEmpty() &&
        !srcProvider->displayName().isEmpty()) {
        QByteArray remote = (cut ? "cut\n" : "copy\n");
        remote += srcProvider->scheme().toUtf8() + "\n";
        remote += srcProvider->displayName().toUtf8() + "\n";
        for (const QString &path : paths)
            remote += path.toUtf8() + "\n";
        mime->setData(QLatin1String(kRemoteClipboardMime), remote);
    }

    return mime;
}

// Frameless-window chrome metrics (see paintEvent / event / changeEvent).
constexpr int kShadowMargin = 16; // translucent margin: drop shadow + resize band
constexpr int kCornerRadius = 8;  // rounded window-corner radius
constexpr int kResizeGrab = 8;    // edge-resize grab band, straddling the content edge

// A saved tab path that points at removable media which is no longer present:
// it lives under a removable mount root (/media, /run/media, /mnt) yet the
// directory doesn't currently exist -- i.e. the stick/drive was unplugged
// while the app was closed. Network/virtual paths (smb://, empty, "/") and
// ordinary local directories are left untouched.
bool isMissingRemovablePath(const QString &path) {
    if (!path.startsWith(QLatin1Char('/')))
        return false; // network/virtual (smb://, archive "/", flat search)
    static const QStringList kRemovableRoots = {
        QStringLiteral("/media/"), QStringLiteral("/run/media/"), QStringLiteral("/mnt/")};
    bool underRemovable = false;
    for (const QString &root : kRemovableRoots) {
        if (path.startsWith(root)) {
            underRemovable = true;
            break;
        }
    }
    if (!underRemovable)
        return false;
    return !QFileInfo(path).isDir();
}
} // namespace

MainWindow::MainWindow(QWidget *parent, qint64 startupElapsedMs, bool collectStartupPhases,
                       const QElapsedTimer *startupClock)
    : QMainWindow(parent), m_collectStartupPhases(collectStartupPhases) {
    if (startupClock)
        m_startupClock = *startupClock;
    m_startupElapsed.start();
    m_startupElapsedOffsetMs = qMax<qint64>(0, startupElapsedMs);
    m_startupTrace.setCollecting(m_collectStartupPhases);
    if (m_collectStartupPhases) {
        m_startupTrace.mark(QStringLiteral("applicationSetupMs"), m_startupElapsedOffsetMs);
        m_startupTrace.mark(QStringLiteral("mainWindowBodyStartedMs"), elapsedSinceStartup());
    }
    // Frameless: we draw our own title bar (see setupMenuAndToolbar / TitleBar)
    // plus a rounded background and soft shadow in paintEvent. The window is
    // translucent so the shadow can fade into nothing at its edges.
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("FileCommander"));
    resize(1200, 700);

    const QByteArray savedGeometry = m_settings.windowGeometry();
    if (!savedGeometry.isEmpty())
        restoreGeometry(savedGeometry);

    m_themeManager = new ThemeManager(this);

    // A self-painted splitter so the divider line runs the full panel height
    // -- up through the breadcrumb and tab row -- clearly separating the two
    // panels. (A stylesheet on the splitter would cascade onto the tables and
    // slow their repaints, so we paint just the handle.)
    auto *splitter = new PanelSplitter(this);
    m_panelSplitter = splitter;
    m_startupTrace.mark(QStringLiteral("panelsConstructionStartedMs"), elapsedSinceStartup());
    QFont initialListFont = Typography::chromeFont(m_settings);
    initialListFont.setPointSize(m_settings.listFontSize());
    m_startupTrace.mark(QStringLiteral("chromeFontResolvedMs"), elapsedSinceStartup());
    m_leftPanel = new FilePanel(initialListFont, splitter);
    m_startupTrace.mark(QStringLiteral("leftPanelConstructedMs"), elapsedSinceStartup());
    m_rightPanel = new FilePanel(initialListFont, splitter);
    m_startupTrace.mark(QStringLiteral("panelsConstructedMs"), elapsedSinceStartup());
    splitter->addWidget(m_leftPanel);
    splitter->addWidget(m_rightPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setHandleWidth(2);
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        FileSystemModel *model = panel->model();
        connect(model, &FileSystemModel::loadStarted, this,
                [this, panel, model] { resetStartupPanelLoad(panel, model->loadGeneration()); });
        connect(model, &FileSystemModel::loadFinished, this,
                [this, panel](int, quint64 generation) { markStartupPanelLoaded(panel, generation); });
    }
    // App-wide filter so Tab out of the preview pane returns to the file list
    // (the list->preview half is driven by FilePanel::switchPanelRequested).
    qApp->installEventFilter(this);

    // (Folder trees are now per-panel, inside each FilePanel.)

    m_functionKeyBar = new FunctionKeyBar(this);
    m_commandBar = new CommandBar(this);
    connect(m_commandBar, &CommandBar::commandSubmitted, this, &MainWindow::runCommand);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_commandBar);
    layout->addWidget(m_functionKeyBar);
    setCentralWidget(central);
    // Give the content its own cursor so it does NOT inherit the window's cursor.
    // The frameless edge-resize sets a resize cursor on the window (below), but
    // the window only receives mouse-moves over its own margin — not over child
    // widgets — so without this the resize cursor set at an edge would persist
    // (inherited) once the pointer moved into the content and never reset.
    central->setCursor(Qt::ArrowCursor);
    // Transparent margin around the content: the drop shadow is painted here,
    // and it doubles as the WM-driven edge-resize band (see event()). Collapsed
    // to 0 when maximized (changeEvent).
    setMouseTracking(true);
    setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    // Coalesces per-step mask updates during an interactive resize into one
    // final XShape call (see resizeEvent / applyRoundedMask).
    m_maskTimer = new QTimer(this);
    m_maskTimer->setSingleShot(true);
    m_maskTimer->setInterval(50);
    connect(m_maskTimer, &QTimer::timeout, this, &MainWindow::applyRoundedMask);
    // Debounce the Ctrl+Q preview: refresh it only after the cursor has been
    // still for a moment, so arrow-scrolling past big archives/videos doesn't
    // kick off (then abandon) an expensive listing/decode for every row.
    m_quickViewDebounce = new QTimer(this);
    m_quickViewDebounce->setSingleShot(true);
    m_quickViewDebounce->setInterval(180);
    connect(m_quickViewDebounce, &QTimer::timeout, this, &MainWindow::updateQuickView);
    // No global status bar: each FilePanel carries its own status strip, so
    // the function-key bar stays the bottom-most widget.

    m_queue = new OperationQueue(this);
    m_startupTrace.mark(QStringLiteral("operationQueueConstructedMs"), elapsedSinceStartup());
    m_queue->setConflictHandler([this](const FileConflict &conflict) {
        // Same reasoning as the error handler below: parent to the progress window (so
        // the window manager stacks this as its owned window) and suppress the progress
        // window's own deferred auto-show while this prompt is up, so neither an
        // unrelated-sibling z-order nor a mid-decision auto-show can cover it.
        QWidget *dialogParent = (m_progressDialog && m_progressDialog->isVisible())
            ? static_cast<QWidget *>(m_progressDialog)
            : static_cast<QWidget *>(this);
        if (m_progressDialog)
            m_progressDialog->suppressAutoShow(true);
        const ErrorAction action = OverwriteConfirmDialog::ask(dialogParent, conflict);
        if (m_progressDialog)
            m_progressDialog->suppressAutoShow(false);
        return action;
    });
    m_queue->setErrorHandler([this](const OperationError &error) {
        // Parent to the (non-modal) transfer progress window when it is on screen so the
        // window manager stacks this modal decision dialog as its owned window and keeps
        // it above the progress window, instead of floating as an unrelated MainWindow
        // sibling that the progress window's own updates can end up covering.
        QWidget *dialogParent = (m_progressDialog && m_progressDialog->isVisible())
            ? static_cast<QWidget *>(m_progressDialog)
            : static_cast<QWidget *>(this);
        // Belt-and-braces alongside WindowStaysOnTopHint above: the progress window's own
        // deferred-show timer (armed for slow operations) can still fire *while* this
        // dialog's nested event loop is running -- e.g. the user takes a moment to read the
        // prompt -- newly showing a sibling window at that point can beat a topmost hint on
        // some window managers. Suppressing it here removes the race outright rather than
        // relying on winning it.
        if (m_progressDialog)
            m_progressDialog->suppressAutoShow(true);
        const ErrorAction action =
            OperationErrorDialog::ask(dialogParent, error, PrivilegeBroker::isAvailable());
        if (m_progressDialog)
            m_progressDialog->suppressAutoShow(false);
        if (action == ErrorAction::Abort) {
            m_operationAbortRequested = true;
            m_operationErrors.clear();
            m_pendingDeletePanel = nullptr;
            m_pendingDeletePaths.clear();
            m_pendingMovePanel = nullptr;
            m_pendingMovePaths.clear();
            m_pendingDestPanel = nullptr;
            m_pendingDestPaths.clear();
            m_queue->abortAll();
            if (m_progressDialog)
                m_progressDialog->dismissAfterAbort();
        }
        return action;
    });
    connect(m_queue, &OperationQueue::started, this,
            [this](const QString &) {
                if (!m_operationAbortRequested)
                    m_operationErrors.clear();
            });
    connect(m_queue, &OperationQueue::finished, this, [this](bool ok) {
        if (m_operationAbortRequested) {
            m_leftPanel->refresh();
            m_rightPanel->refresh();
            if (!m_queue->isBusy() && m_queue->queuedCount() == 0)
                m_operationAbortRequested = false;
            return;
        }

        bool handledPlan = false;

        if (!ok) {
            if (m_pendingDeletePanel) {
                m_pendingDeletePanel->refresh();
                m_pendingDeletePanel = nullptr;
                m_pendingDeletePaths.clear();
                handledPlan = true;
            }
            if (m_pendingMovePanel) {
                m_pendingMovePanel->refresh();
                m_pendingMovePanel = nullptr;
                m_pendingMovePaths.clear();
                handledPlan = true;
            }
            if (m_pendingDestPanel) {
                m_pendingDestPanel->refresh();
                m_pendingDestPanel = nullptr;
                m_pendingDestPaths.clear();
                handledPlan = true;
            }
        }

        if (ok && m_pendingDeletePanel) {
            // A delete just finished. On a local tab that means dropping the
            // vanished rows in place and moving the cursor onto the next file
            // rather than rescanning (which would reset the selection to the
            // first row); on a network/archive tab the panel has to go and ask
            // instead -- see FilePanel::settleAfterRemoval.
            FilePanel *panel = m_pendingDeletePanel;
            m_pendingDeletePanel = nullptr;
            panel->settleAfterRemoval(m_pendingDeletePaths);
            m_pendingDeletePaths.clear();
            // The other panel may show the same directory, so refresh it fully.
            FilePanel *other = (panel == m_leftPanel) ? m_rightPanel : m_leftPanel;
            other->refresh();
            handledPlan = true;
        }

        if (ok && m_pendingMovePanel) {
            // A move just finished: the source files vanished, so the source
            // panel settles the same way a delete does.
            FilePanel *panel = m_pendingMovePanel;
            m_pendingMovePanel = nullptr;
            panel->settleAfterRemoval(m_pendingMovePaths);
            m_pendingMovePaths.clear();
            handledPlan = true;
        }

        if (ok && m_pendingDestPanel) {
            // A copy or move just landed files in this panel: refresh it and
            // select the freshly-arrived file(s), leaving every other panel
            // untouched.
            FilePanel *panel = m_pendingDestPanel;
            m_pendingDestPanel = nullptr;
            for (const QString &p : m_pendingDestPaths)
                panel->selectPathAfterReload(p);
            m_pendingDestPaths.clear();
            panel->refresh();
            handledPlan = true;
        }

        if (!handledPlan) {
            m_leftPanel->refresh();
            m_rightPanel->refresh();
        }
        // Report all per-file failures once, not one modal per error.
        if (!m_operationErrors.isEmpty()) {
            const int shown = qMin(m_operationErrors.size(), 20);
            QString text = m_operationErrors.mid(0, shown).join(QLatin1Char('\n'));
            if (m_operationErrors.size() > shown)
                text += tr("\n... and %1 more.").arg(m_operationErrors.size() - shown);
            ttc::warning(this, tr("Operation Error"), text);
            m_operationErrors.clear();
        }
    });
    connect(m_queue, &OperationQueue::errorOccurred, this, [this](const QString &msg) {
        if (!m_operationAbortRequested)
            m_operationErrors.append(msg);
    });

    // Restore persisted view state before the first scan so it takes effect
    // immediately: hidden-files preference and the shared column layout/sort.
    const bool showHidden = m_settings.showHiddenFiles();
    m_leftPanel->model()->setShowHiddenFiles(showHidden);
    m_rightPanel->model()->setShowHiddenFiles(showHidden);
    const bool archiveAsFolder = m_settings.archiveAsFolder();
    m_leftPanel->setArchiveAsFolder(archiveAsFolder);
    m_rightPanel->setArchiveAsFolder(archiveAsFolder);
    // Per-side column layout (base widths + hidden mask + sort), stored
    // independently for each panel. When absent (fresh profile, or upgrading from
    // the old single shared header blob) the panel simply content-fits on first
    // load -- the legacy widths are intentionally NOT migrated, since they came
    // from a proportional auto-scale rather than deliberate tuning.
    auto restorePanelColumns = [this](FilePanel *panel, const QString &side) {
        const QString csv = m_settings.columnBaseWidths(side);
        if (csv.isEmpty())
            return;
        QVector<int> widths;
        for (const QString &part : csv.split(QLatin1Char(','), Qt::SkipEmptyParts))
            widths.append(part.toInt());
        const int sc = m_settings.sortColumn(side);
        panel->view()->restoreColumnLayout(widths, m_settings.hiddenColumnsMask(side), sc,
                                           static_cast<Qt::SortOrder>(m_settings.sortOrder(side)));
    };
    restorePanelColumns(m_leftPanel, QStringLiteral("left"));
    restorePanelColumns(m_rightPanel, QStringLiteral("right"));
    m_startupTrace.mark(QStringLiteral("panelPreferencesRestoredMs"), elapsedSinceStartup());

    // Optional bars + splitter layout. Applied before buildTitleBarMenus() so
    // the Interface-menu checkmarks (which read the widgets' visibility) match.
    m_commandBar->setVisible(m_settings.showCommandBar());
    m_functionKeyBar->setVisible(m_settings.showFunctionKeyBar());
    applyInterfaceTypography();
    m_startupTrace.mark(QStringLiteral("interfaceTypographyAppliedMs"), elapsedSinceStartup());
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        panel->setTabBarVisible(m_settings.showTabBar());
        panel->setDirectoryTreeVisible(m_settings.showFolderTree());
    }
    m_startupTrace.mark(QStringLiteral("panelVisibilityRestoredMs"), elapsedSinceStartup());
    if (const QByteArray s = m_settings.panelSplitterState(); !s.isEmpty())
        m_panelSplitter->restoreState(s);
    m_startupTrace.mark(QStringLiteral("viewSettingsRestoredMs"), elapsedSinceStartup());

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    SessionPanelData leftSession, rightSession;
    const bool sessionLoaded = SessionManager::load(leftSession, rightSession);
    m_startupTrace.mark(QStringLiteral("sessionDataLoadedMs"), elapsedSinceStartup());
    if (sessionLoaded) {
        // Network tabs are not restored at all -- reconnecting during startup
        // blocks on an unreachable server or pops a password dialog before the
        // window is even usable. See SessionManager::dropNetworkTabs().
        SessionManager::dropNetworkTabs(leftSession);
        SessionManager::dropNetworkTabs(rightSession);
        // Drop tabs whose removable medium is gone (device unplugged while the
        // app was closed), keeping the active-tab index pointing at a survivor.
        auto buildTabs = [](const SessionPanelData &s, int &activeOut) {
            QVector<FilePanel::RestoredTab> tabs;
            int active = 0;
            for (int i = 0; i < s.tabs.size(); ++i) {
                if (isMissingRemovablePath(s.tabs.at(i).path))
                    continue;
                if (i < s.activeTab)
                    ++active; // this kept tab sits before the old active one
                tabs.append({s.tabs.at(i).path, s.tabs.at(i).selectedFiles,
                             s.tabs.at(i).computerView});
            }
            activeOut = qBound(0, active, qMax(0, tabs.size() - 1));
            return tabs;
        };
        int leftActive = 0, rightActive = 0;
        const auto leftTabs = buildTabs(leftSession, leftActive);
        const auto rightTabs = buildTabs(rightSession, rightActive);
        if (leftTabs.isEmpty())
            m_leftPanel->navigateTo(home);
        else
            m_leftPanel->restoreTabs(leftTabs, leftActive);
        if (rightTabs.isEmpty())
            m_rightPanel->navigateTo(home);
        else
            m_rightPanel->restoreTabs(rightTabs, rightActive);
        // A restored tab that was on the computer view has fallen back to its
        // directory, because restoreTabs runs before computerViewRequested is
        // connected. Note it and enter the view once that wiring exists.
        for (FilePanel *panel : {m_leftPanel, m_rightPanel})
            if (panel->activeTabWantsComputerView())
                m_startupComputerViewPanels.append(panel);
    } else {
        // First launch: the left pane opens on the computer view -- drives, the
        // user folders, whatever is plugged in -- so the first thing shown is a
        // way to everywhere, and the right pane on the home directory to copy
        // to and from. The view itself is entered further down, once the signal
        // that assembles its rows exists.
        m_leftPanel->navigateTo(home);
        m_rightPanel->navigateTo(home);

        m_startupComputerViewPanels.append(m_leftPanel);
    }
    m_startupTrace.mark(QStringLiteral("sessionNavigationDispatchedMs"), elapsedSinceStartup());

    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        // The server wants credentials: prompt, then hand them to that panel's
        // model to retry the connection with a username/password.
        FileSystemModel *model = panel->model();
        connect(model, &FileSystemModel::networkAuthRequired, this,
                [this, model, panel](const QString &host, const QString &error) {
                    QString user, pass;
                    if (promptCredentials(host, &user, &pass, error))
                        model->provideCredentials(user, pass);
                    else
                        // Cancelled: don't leave a blank, connected-looking tab --
                        // show a login prompt the user can click to try again.
                        panel->showLoginPrompt();
                });
        // An inline rename on a remote tab: run it on the transfer pool (never the
        // GUI thread). Uses the panel's live provider; the queue's finished handler
        // refreshes the panels and its error path reports any failure.
        connect(model, &FileSystemModel::remoteRenameRequested, this,
                [this, model](const QString &oldPath, const QString &newName) {
                    if (model->provider()) {
                        ensureTransferProgressDialog();
                        m_queue->enqueueProviderRename(model->providerPtr(), oldPath, newName);
                    }
                });
        // The "登录" link on a cancelled-auth tab: prompt again and retry.
        connect(panel, &FilePanel::loginRequested, this, [this, model](FilePanel *p) {
            QString user, pass;
            if (promptCredentials(p->currentPath(), &user, &pass))
                model->provideCredentials(user, pass);
            else
                p->showLoginPrompt();
        });
        connect(panel, &FilePanel::panelActivated, this, &MainWindow::setActivePanel);
        connect(panel, &FilePanel::switchPanelRequested, this, [this, panel]() {
            // With the preview active, the "other" panel is hidden behind it, so
            // Tab from the visible (active) list moves into the preview pane
            // instead. Tab back out of the preview is handled in eventFilter().
            if (m_quickViewActive && panel == m_activePanel) {
                m_quickView->focusPreview();
                return;
            }
            FilePanel *other = otherPanel(panel);
            other->view()->setFocus();
            setActivePanel(other);
        });
        connect(panel, &FilePanel::shortcutMenuRequested, this, &MainWindow::showShortcutMenu);
        connect(panel, &FilePanel::favoritesMenuRequested, this, &MainWindow::showFavoritesMenu);
        connect(panel, &FilePanel::computerViewRequested, this, &MainWindow::showComputerView);
        connect(panel, &FilePanel::computerEntryActivated, this,
                &MainWindow::openComputerEntry);
        connect(panel, &FilePanel::pathChanged, this, [this, panel](const QString &path) {
            if (panel == m_activePanel)
                m_commandBar->setDirectory(path);
        });
        connect(panel->view(), &FileListView::filesDropped, this, &MainWindow::handleFilesDropped);
        auto configureIconView = [this, panel](IconFileView *iconView) {
            connect(iconView, &IconFileView::filesDropped, this,
                    &MainWindow::handleFilesDropped);
            iconView->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(iconView, &QWidget::customContextMenuRequested, this,
                    [this, panel, iconView](const QPoint &pos) {
                        setActivePanel(panel);
                        if (iconView->indexAt(pos).isValid())
                            showFileContextMenu(panel, pos);
                        else
                            showBlankContextMenu(panel, pos);
                    });
        };
        connect(panel, &FilePanel::iconViewCreated, this, configureIconView);
        if (IconFileView *iconView = panel->iconView())
            configureIconView(iconView);
        connect(panel->view()->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this]() {
                    if (m_quickViewActive)
                        m_quickViewDebounce->start(); // coalesce rapid cursor moves
                });
        connect(panel->model(), &FileSystemModel::renameFailed, this, [this](const QString &msg) {
            ttc::warning(this, tr("Rename"), msg);
        });
        connect(panel->model(), &FileSystemModel::renamed, this,
                [this, panel](const QString &oldPath, const QString &newPath) {
                    m_lastUndo = UndoRecord{};
                    m_lastUndo.type = UndoRecord::Rename;
                    m_lastUndo.fromPath = newPath;
                    m_lastUndo.toName = QFileInfo(oldPath).fileName();
                    // Remember the backend so undo of a remote rename goes back
                    // through the provider, not the local filesystem.
                    FileProvider *prov = panel->model()->provider();
                    m_lastUndo.provider = (prov == LocalFileProvider::instance())
                                              ? std::shared_ptr<FileProvider>()
                                              : panel->model()->providerPtr();
                    // Keep the cursor on the renamed entry after the reload
                    // (fired synchronously before the model's rescan starts).
                    panel->selectPathAfterReload(newPath);
                });

        // Both the list and thumbnail (icon) views get the same context menu;
        // showFileContextMenu()/showBlankContextMenu() operate on the panel's
        // ACTIVE view (whichever is visible), and the two views share a
        // selection model, so right-clicking either behaves identically.
        panel->view()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(panel->view(), &QWidget::customContextMenuRequested, this,
                [this, panel](const QPoint &pos) {
                    setActivePanel(panel);
                    if (panel->view()->indexAt(pos).isValid())
                        showFileContextMenu(panel, pos);
                    else
                        showBlankContextMenu(panel, pos);
                });
        connect(panel, &FilePanel::openRequested, this, [this, panel](const QString &path) {
            // Double-click opens with the system's MIME-associated application.
            // (Directories and local archives are handled earlier in
            // FilePanel::onActivated and never reach here; F3 is the in-app
            // viewer.) The emitting panel decides how to reach the file: on a
            // network tab `path` belongs to the provider, not the filesystem.
            openWithAssociatedApp(panel, path);
        });
        connect(panel, &FilePanel::archiveDownloadRequested, this, &MainWindow::browseRemoteArchive);
        // An archive entry finished extracting. Show it only if the cursor is
        // still on it and this panel is still the one being previewed -- the
        // user may have moved on, or switched panels, while it ran.
        connect(panel, &FilePanel::previewExtracted, this,
                [this, panel](const QString &entryPath, const QString &localPath) {
                    if (!m_quickViewActive || m_activePanel != panel)
                        return;
                    if (panel->currentEntryPath() != entryPath)
                        return;
                    m_quickView->showFile(localPath);
                });
    }

    // Now that computerViewRequested is connected, the panels noted above can
    // actually be put into the computer view -- a first launch's left pane, and
    // any tab the last session left on it.
    for (FilePanel *panel : m_startupComputerViewPanels)
        showComputerView(panel);
    m_startupComputerViewPanels.clear();

    setActivePanel(m_leftPanel);
    m_leftPanel->view()->setFocus();
    setTabOrder(m_leftPanel->view(), m_rightPanel->view());
    setTabOrder(m_rightPanel->view(), m_leftPanel->view());

    setupShortcuts();
    buildTitleBarMenus();
    m_startupTrace.mark(QStringLiteral("shortcutsTitleBarReadyMs"), elapsedSinceStartup());

    // Per-side, per-mode view scale (status-bar -/+ buttons): restore whatever
    // was last saved, then persist again whenever a panel's -/+ click changes
    // it. Applied after startup typography since it can override the
    // font-derived thumbnail/row size.
    m_leftPanel->setThumbnailIconSize(m_settings.thumbnailIconSize(QStringLiteral("left")));
    m_leftPanel->setListRowHeight(m_settings.listRowHeight(QStringLiteral("left")));
    m_rightPanel->setThumbnailIconSize(m_settings.thumbnailIconSize(QStringLiteral("right")));
    m_rightPanel->setListRowHeight(m_settings.listRowHeight(QStringLiteral("right")));

    connect(m_leftPanel, &FilePanel::viewScaleChanged, this, [this]() {
        m_settings.setThumbnailIconSize(QStringLiteral("left"), m_leftPanel->thumbnailIconSize());
        m_settings.setListRowHeight(QStringLiteral("left"), m_leftPanel->listRowHeight());
    });
    connect(m_rightPanel, &FilePanel::viewScaleChanged, this, [this]() {
        m_settings.setThumbnailIconSize(QStringLiteral("right"), m_rightPanel->thumbnailIconSize());
        m_settings.setListRowHeight(QStringLiteral("right"), m_rightPanel->listRowHeight());
    });

    // Apply the startup stylesheet once, after every visible widget exists but
    // before main() can show the window. Runtime changes still use applyTheme().
    m_startupTrace.mark(QStringLiteral("startupThemeApplyStartedMs"), elapsedSinceStartup());
    m_themeManager->apply(m_settings.theme(), m_settings.phosphorImages(),
                          m_settings.phosphorPreview());
    // The widgets above were built before that stylesheet existed, so any of
    // their artwork recoloured from the palette is now stale -- exactly as it
    // would be after a runtime theme change, and fixed the same way.
    refreshThemedArtwork();
    m_startupTrace.mark(QStringLiteral("startupThemeApplyFinishedMs"), elapsedSinceStartup());
    // Installing a stylesheet swaps the application style, and that resets
    // QApplication::font() back to the platform default -- measured as SimSun 12
    // where the interface font was Microsoft YaHei UI 9. Widgets that already
    // hold an explicit font (this window, and so the menus under it) survive it,
    // which is why the damage was invisible until something resolved a font
    // straight from the application: the font-size rows in the Interface menu are
    // built later, and QWidgetAction briefly leaves them parentless, so they
    // picked up the stale application font and rendered a size and family apart
    // from every entry beside them. Re-applying afterwards is what the settings
    // path was doing implicitly -- which is exactly why adjusting the size once
    // "fixed" it for the session, and a restart brought it back.
    applyInterfaceTypography();
}

QString MainWindow::commandText(const QString &id, const QString &label) const {
    if (!m_settings.showShortcutLabels())
        return label;
    const QKeySequence sequence = m_commands.sequence(id);
    const QString shortcut = sequence.toString(QKeySequence::NativeText);
    return shortcut.isEmpty() ? label : label + QLatin1Char('\t') + shortcut;
}

QAction *MainWindow::addCommandAction(QMenu *menu, const QString &id, const QString &label,
                                      std::function<void()> handler) {
    if (!handler)
        handler = m_commands.handler(id);
    QAction *action = menu->addAction(commandText(id, label));
    connect(action, &QAction::triggered, this, [handler]() {
        if (handler)
            handler();
    });
    return action;
}

void MainWindow::buildTitleBarMenus() {
    // Re-runnable (called again on a live language change): drop the previous
    // menus so we don't leak them. setMenuWidget() deletes the old title bar.
    delete m_toolsMenu;
    delete m_configMenu;
    delete m_interfaceMenu;

    auto *toolsMenu = new QMenu(tr("&Tools"), this);
    Typography::applyChromeFont(toolsMenu, m_settings);
    m_toolsMenu = toolsMenu;
    connect(toolsMenu, &QMenu::aboutToShow, this, [this, toolsMenu] {
        if (!toolsMenu->isEmpty())
            return;
        addCommandAction(toolsMenu, QStringLiteral("notepad"), tr("Quick Notepad"));
        addCommandAction(toolsMenu, QStringLiteral("checksums"), tr("Calculate Checksums"));
        addCommandAction(toolsMenu, QStringLiteral("secureWipe"), tr("Secure Wipe"));
        addCommandAction(toolsMenu, QStringLiteral("compareFiles"), tr("Compare Files"));
    });

    auto *configMenu = new QMenu(tr("Con&fig"), this);
    Typography::applyChromeFont(configMenu, m_settings);
    m_configMenu = configMenu;
    connect(configMenu, &QMenu::aboutToShow, this, [this, configMenu] {
    if (!configMenu->isEmpty()) {
        syncConfigMenuState();
        return;
    }
    addCommandAction(configMenu, QStringLiteral("keyboardShortcuts"), tr("Keyboard Shortcuts"));
    addCommandAction(configMenu, QStringLiteral("connectionManager"),
                     tr("Manage Network Connections"));
    configMenu->addSeparator();
    // Checked means "browse into it", which is what the label says. It used to
    // be inverted -- the entry read "Directly Open Archives" while ticking it
    // set archiveAsFolder=false, i.e. turned browsing OFF -- so anyone who
    // ticked it to get into archives got the opposite. The setting key keeps
    // its meaning (true = browse as a folder); only the checkbox now agrees
    // with it, and the label says which of the two things it does.
    QAction *directArchives = configMenu->addAction(
        commandText(QStringLiteral("toggleDirectArchives"), tr("Open Archives as Folders")));
    directArchives->setObjectName(QStringLiteral("configDirectArchivesAction"));
    directArchives->setCheckable(true);
    directArchives->setChecked(m_settings.archiveAsFolder());
    connect(directArchives, &QAction::toggled, this, [this](bool on) {
        m_settings.setArchiveAsFolder(on);
        m_leftPanel->setArchiveAsFolder(on);
        m_rightPanel->setArchiveAsFolder(on);
    });
    QAction *noConfirm = configMenu->addAction(
        commandText(QStringLiteral("toggleDeleteConfirmation"), tr("Skip Trash Delete Confirmation")));
    noConfirm->setObjectName(QStringLiteral("configDeleteConfirmationAction"));
    noConfirm->setCheckable(true);
    noConfirm->setChecked(!m_settings.confirmDelete());
    noConfirm->setToolTip(
        tr("Skip confirmation only when deleting local files to the trash. "
           "Shift+Delete and remote deletes always require confirmation."));
    connect(noConfirm, &QAction::toggled, this,
            [this](bool on) { m_settings.setConfirmDelete(!on); });
    QAction *autoUpdate = configMenu->addAction(
        commandText(QStringLiteral("toggleAutoUpdate"), tr("Automatic Update Check")));
    autoUpdate->setObjectName(QStringLiteral("configAutoUpdateAction"));
    autoUpdate->setCheckable(true);
    autoUpdate->setChecked(m_settings.autoUpdateCheck());
    connect(autoUpdate, &QAction::toggled, this,
            [this](bool on) { m_settings.setAutoUpdateCheck(on); });
    syncConfigMenuState();
    });

    auto *interfaceMenu = new QMenu(tr("&Interface"), this);
    Typography::applyChromeFont(interfaceMenu, m_settings);
    m_interfaceMenu = interfaceMenu;
    connect(interfaceMenu, &QMenu::aboutToShow, this, [this, interfaceMenu] {
    if (!interfaceMenu->isEmpty()) {
        syncInterfaceMenuState();
        return;
    }
    QMenu *themeMenu = interfaceMenu->addMenu(tr("&Theme"));
    Typography::applyChromeFont(themeMenu, m_settings);
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    struct ThemeEntry {
        Settings::Theme theme;
        QString label;
    };
    const ThemeEntry themeEntries[] = {
        {Settings::Theme::Auto, tr("Auto")},
        {Settings::Theme::Light, tr("Light")},
        {Settings::Theme::Dark, tr("Dark")},
        {Settings::Theme::Crt, tr("Green CRT")},
    };
    for (const auto &entry : themeEntries) {
        QAction *action = themeMenu->addAction(entry.label);
        action->setObjectName(
            QStringLiteral("interfaceThemeAction%1").arg(static_cast<int>(entry.theme)));
        action->setCheckable(true);
        action->setChecked(m_settings.theme() == entry.theme);
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, theme = entry.theme]() { setTheme(theme); });
    }

    // Content recolouring, under every theme -- each recolours to a bright
    // member of its own palette (see ThemeManager::apply).
    themeMenu->addSeparator();
    // Two switches, not one. The file list's pictures and the picture someone
    // opened to LOOK at are different decisions -- recolouring a wall of small
    // thumbnails is decoration, recolouring the image in the preview pane
    // changes what is being examined.
    QAction *imageColours = themeMenu->addAction(tr("Image Colours Follow Theme"));
    imageColours->setObjectName(QStringLiteral("interfacePhosphorImagesAction"));
    imageColours->setCheckable(true);
    imageColours->setChecked(m_settings.phosphorImages());
    imageColours->setToolTip(
        tr("Recolour the file list's icons and thumbnails to the theme's hue. "
           "The preview pane is not affected."));
    connect(imageColours, &QAction::triggered, this,
            &MainWindow::setPhosphorImages);

    QAction *previewColours = themeMenu->addAction(tr("Preview Colours Follow Theme"));
    previewColours->setObjectName(QStringLiteral("interfacePhosphorPreviewAction"));
    previewColours->setCheckable(true);
    previewColours->setChecked(m_settings.phosphorPreview());
    previewColours->setToolTip(
        tr("Recolour images, video and documents shown in the preview pane to the "
           "theme's hue."));
    connect(previewColours, &QAction::triggered, this,
            &MainWindow::setPhosphorPreview);

    QMenu *languageMenu = interfaceMenu->addMenu(tr("&Language"));
    Typography::applyChromeFont(languageMenu, m_settings);
    auto *languageGroup = new QActionGroup(this);
    languageGroup->setExclusive(true);
    // Discovered from the bundled catalogs plus the user's external translations
    // dir, so a dropped-in ttc_<code>.qm shows up here without any code change.
    // Each is listed under its own native name; "Auto" follows the system locale.
    for (const auto &entry : TranslationManager::available()) {
        QAction *action = languageMenu->addAction(entry.second);
        action->setCheckable(true);
        action->setChecked(m_settings.language() == entry.first);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, code = entry.first]() { setLanguage(code); });
    }

    // File-list font size: caption + "[−] <number> [+]". The number field is
    // edited directly in place; the − / + buttons step it. Range 8..16.
    {
        auto *fontWidget = new QWidget(interfaceMenu);
        Typography::applyChromeFont(fontWidget, m_settings);
        auto *fontLayout = new QHBoxLayout(fontWidget);
        fontLayout->setContentsMargins(20, 2, 12, 2);
        fontLayout->setSpacing(4);

        auto *caption = new QLabel(tr("File List Font Size"), fontWidget);

        auto *minusBtn = new QToolButton(fontWidget);
        minusBtn->setText(QStringLiteral("−"));
        minusBtn->setAutoRaise(true);
        minusBtn->setFocusPolicy(Qt::NoFocus);

        auto *sizeEdit = new QLineEdit(fontWidget);
        sizeEdit->setValidator(new QIntValidator(8, 16, sizeEdit));
        sizeEdit->setAlignment(Qt::AlignCenter);
        sizeEdit->setFixedWidth(36);
        sizeEdit->setText(QString::number(m_settings.listFontSize()));
        sizeEdit->setToolTip(tr("Type a size, or use − / + (8-16)"));

        auto *plusBtn = new QToolButton(fontWidget);
        plusBtn->setText(QStringLiteral("+"));
        plusBtn->setAutoRaise(true);
        plusBtn->setFocusPolicy(Qt::NoFocus);

        fontLayout->addWidget(caption);
        fontLayout->addStretch(1);
        fontLayout->addWidget(minusBtn);
        fontLayout->addWidget(sizeEdit);
        fontLayout->addWidget(plusBtn);

        // Applies a clamped size to settings + both panels and reflects it back
        // into the field (so an out-of-range typed value snaps into view).
        auto apply = [this, sizeEdit](int pt) {
            pt = qBound(8, pt, 16);
            if (pt != m_settings.listFontSize()) {
                m_settings.setListFontSize(pt);
                m_leftPanel->setListFontSize(pt);
                m_rightPanel->setListFontSize(pt);
                if (m_quickView)
                    m_quickView->setContentFontSize(pt);
            }
            const QString s = QString::number(pt);
            if (sizeEdit->text() != s)
                sizeEdit->setText(s);
        };
        connect(minusBtn, &QToolButton::clicked, this,
                [this, apply]() { apply(m_settings.listFontSize() - 1); });
        connect(plusBtn, &QToolButton::clicked, this,
                [this, apply]() { apply(m_settings.listFontSize() + 1); });
        connect(sizeEdit, &QLineEdit::textEdited, this,
                [apply](const QString &t) { if (!t.isEmpty()) apply(t.toInt()); });

        auto *fontAction = new QWidgetAction(interfaceMenu);
        fontAction->setDefaultWidget(fontWidget);
        interfaceMenu->addAction(fontAction);
    }

    {
        auto *fontWidget = new QWidget(interfaceMenu);
        Typography::applyChromeFont(fontWidget, m_settings);
        auto *fontLayout = new QHBoxLayout(fontWidget);
        fontLayout->setContentsMargins(20, 2, 12, 2);
        fontLayout->setSpacing(4);

        auto *caption = new QLabel(tr("Menu Font Size"), fontWidget);
        auto *minusBtn = new QToolButton(fontWidget);
        minusBtn->setText(QStringLiteral("-"));
        minusBtn->setAutoRaise(true);
        minusBtn->setFocusPolicy(Qt::NoFocus);

        auto *sizeEdit = new QLineEdit(fontWidget);
        sizeEdit->setValidator(new QIntValidator(8, 16, sizeEdit));
        sizeEdit->setAlignment(Qt::AlignCenter);
        sizeEdit->setFixedWidth(36);
        sizeEdit->setText(QString::number(m_settings.menuFontSize()));
        sizeEdit->setToolTip(tr("Type a size, or use - / + (8-16)"));

        auto *plusBtn = new QToolButton(fontWidget);
        plusBtn->setText(QStringLiteral("+"));
        plusBtn->setAutoRaise(true);
        plusBtn->setFocusPolicy(Qt::NoFocus);

        fontLayout->addWidget(caption);
        fontLayout->addStretch(1);
        fontLayout->addWidget(minusBtn);
        fontLayout->addWidget(sizeEdit);
        fontLayout->addWidget(plusBtn);

        auto apply = [this, sizeEdit](int pt) {
            pt = qBound(8, pt, 16);
            if (pt != m_settings.menuFontSize()) {
                m_settings.setMenuFontSize(pt);
                // Deliberately NOT calling buildTitleBarMenus() here (unlike
                // chooseGlobalFont(), where the font dialog has already closed this
                // menu by the time it runs): this +/- stepper lives inside the
                // still-open interfaceMenu via a QWidgetAction, and rebuilding the
                // menu destroys and replaces that exact widget out from under an
                // in-flight click -- observed as the menu closing on its own after
                // one step, and occasionally both buttons going dead (stuck on the
                // old, now-detached widget instance). applyInterfaceTypography()
                // already reaches this open menu live: it calls
                // Typography::applyApplicationFont(), and MenuChromeSynchronizer
                // (installed on this exact menu when this widget's font was first
                // set, see Typography::applyChromeFont) re-syncs its embedded
                // widget actions on the resulting ApplicationFontChange, with no
                // rebuild needed.
                applyInterfaceTypography();
            }
            const QString text = QString::number(pt);
            if (sizeEdit->text() != text)
                sizeEdit->setText(text);
        };
        connect(minusBtn, &QToolButton::clicked, this,
                [this, apply]() { apply(m_settings.menuFontSize() - 1); });
        connect(plusBtn, &QToolButton::clicked, this,
                [this, apply]() { apply(m_settings.menuFontSize() + 1); });
        connect(sizeEdit, &QLineEdit::textEdited, this,
                [apply](const QString &text) { if (!text.isEmpty()) apply(text.toInt()); });

        auto *fontAction = new QWidgetAction(interfaceMenu);
        fontAction->setDefaultWidget(fontWidget);
        interfaceMenu->addAction(fontAction);
    }

    interfaceMenu->addAction(commandText(QStringLiteral("chooseFont"), tr("Choose Font")),
                             this, &MainWindow::chooseGlobalFont);
    interfaceMenu->addSeparator();
    QAction *showFnBar = interfaceMenu->addAction(
        commandText(QStringLiteral("toggleFunctionKeyBar"), tr("Show Function Key Bar")));
    showFnBar->setObjectName(QStringLiteral("interfaceFunctionKeyBarAction"));
    showFnBar->setCheckable(true);
    showFnBar->setChecked(!m_functionKeyBar->isHidden());
    connect(showFnBar, &QAction::toggled, this, [this](bool on) {
        m_functionKeyBar->setVisible(on);
        m_settings.setShowFunctionKeyBar(on);
    });
    QAction *showCmdBar = interfaceMenu->addAction(
        commandText(QStringLiteral("toggleCommandBar"), tr("Show Command Bar")));
    showCmdBar->setObjectName(QStringLiteral("interfaceCommandBarAction"));
    showCmdBar->setCheckable(true);
    showCmdBar->setChecked(!m_commandBar->isHidden());
    connect(showCmdBar, &QAction::toggled, this, [this](bool on) {
        m_commandBar->setVisible(on);
        m_settings.setShowCommandBar(on);
    });
    QAction *showTabBar = interfaceMenu->addAction(
        commandText(QStringLiteral("toggleTabBar"), tr("Show File Tab Bar")));
    showTabBar->setObjectName(QStringLiteral("interfaceTabBarAction"));
    showTabBar->setCheckable(true);
    showTabBar->setChecked(m_settings.showTabBar());
    connect(showTabBar, &QAction::toggled, this, [this](bool on) {
        m_leftPanel->setTabBarVisible(on);
        m_rightPanel->setTabBarVisible(on);
        m_settings.setShowTabBar(on);
    });
    QAction *showShortcutLabels = interfaceMenu->addAction(
        commandText(QStringLiteral("toggleShortcutLabels"), tr("Display Shortcut Labels")));
    showShortcutLabels->setObjectName(QStringLiteral("interfaceShortcutLabelsAction"));
    showShortcutLabels->setCheckable(true);
    showShortcutLabels->setChecked(m_settings.showShortcutLabels());
    connect(showShortcutLabels, &QAction::toggled, this,
            [this](bool on) { m_settings.setShowShortcutLabels(on); });
    syncInterfaceMenuState();
    // The embedded font-size rows are built here, and until now they only ever
    // reached their final state on the *next* font change: the descendant pass
    // inside applyChromeFont() runs when the row is still empty (its caption,
    // field and buttons are created immediately after), and the row's
    // QWidgetAction -- which is what QMenu measures the entry with -- was never
    // given the menu font at all. That left these two rows sized differently
    // from every plain entry beside them on a freshly started app, and corrected
    // itself only once the user touched the setting. Running the same sync the
    // change path runs makes the first open identical to every later one.
    Typography::refreshOpenMenuChrome(interfaceMenu);
    });

    // Embed the menus in our self-drawn title bar (app icon + menu buttons +
    // window buttons), placed where the menu bar would normally sit.
    m_titleBar = new TitleBar(this, {interfaceMenu, toolsMenu, configMenu});
    m_titleBar->setCursor(Qt::ArrowCursor); // don't inherit the window resize cursor
    setMenuWidget(m_titleBar);
    // Clicking the title-bar "New Version" badge opens the pending-update dialog.
    connect(m_titleBar, &TitleBar::updateRequested, this, &MainWindow::showUpdateDialog);

    // The recurring half of the update check. The FIRST check is deferred with
    // the rest of the background services below, but arming the timer costs
    // nothing and belongs here: whether the app re-checks tomorrow should not
    // depend on the startup batch having run.
    auto *updateCheckTimer = new QTimer(this);
    updateCheckTimer->setObjectName(QStringLiteral("ScheduledUpdateCheck"));
    updateCheckTimer->setInterval(kUpdateCheckTickMs);
    connect(updateCheckTimer, &QTimer::timeout, this,
            &MainWindow::maybeRunScheduledUpdateCheck);
    updateCheckTimer->start();

    // Device enumeration, SMB discovery and update checks are useful background
    // services, but none are required for the first visible frame.
}

void MainWindow::syncConfigMenuState() {
    if (!m_configMenu)
        return;

    const auto syncChecked = [this](const QString &name, bool checked) {
        if (QAction *action = m_configMenu->findChild<QAction *>(name)) {
            QSignalBlocker block(action);
            action->setChecked(checked);
        }
    };
    syncChecked(QStringLiteral("configDirectArchivesAction"), m_settings.archiveAsFolder());
    syncChecked(QStringLiteral("configDeleteConfirmationAction"), !m_settings.confirmDelete());
    syncChecked(QStringLiteral("configAutoUpdateAction"), m_settings.autoUpdateCheck());
}

void MainWindow::syncInterfaceMenuState() {
    if (!m_interfaceMenu)
        return;

    const auto syncChecked = [this](const QString &name, bool checked) {
        if (QAction *action = m_interfaceMenu->findChild<QAction *>(name)) {
            QSignalBlocker block(action);
            action->setChecked(checked);
        }
    };
    if (QAction *themeAction = m_interfaceMenu->findChild<QAction *>(
            QStringLiteral("interfaceThemeAction%1").arg(static_cast<int>(m_settings.theme())))) {
        // QActionGroup updates its current action from QAction::toggled. Theme
        // handlers use triggered, so this cannot re-enter setTheme().
        if (QActionGroup *group = themeAction->actionGroup();
            group && group->checkedAction() != themeAction && themeAction->isChecked()) {
            themeAction->setChecked(false);
        }
        themeAction->setChecked(true);
    }
    syncChecked(QStringLiteral("interfacePhosphorImagesAction"), m_settings.phosphorImages());
    syncChecked(QStringLiteral("interfacePhosphorPreviewAction"), m_settings.phosphorPreview());
    syncChecked(QStringLiteral("interfaceFunctionKeyBarAction"), !m_functionKeyBar->isHidden());
    syncChecked(QStringLiteral("interfaceCommandBarAction"), !m_commandBar->isHidden());
    syncChecked(QStringLiteral("interfaceTabBarAction"), m_settings.showTabBar());
    syncChecked(QStringLiteral("interfaceShortcutLabelsAction"), m_settings.showShortcutLabels());
}

QJsonObject MainWindow::startupMetrics() const {
    QJsonObject metrics = {{QStringLiteral("visibleMs"), m_startupVisibleMs},
                           {QStringLiteral("panelsLoadedMs"), m_startupPanelsLoadedMs},
                           {QStringLiteral("interactiveMs"), m_startupInteractiveMs}};
    if (m_collectStartupPhases) {
        // Every phase, under the name it was recorded with, in the order it
        // happened -- which is what the probe's monotonicity check needs.
        const QJsonObject phases = m_startupTrace.toJson();
        for (auto it = phases.constBegin(); it != phases.constEnd(); ++it)
            metrics.insert(it.key(), it.value());
        metrics.insert(QStringLiteral("firstShowMs"), m_startupVisibleMs);
        metrics.insert(QStringLiteral("readinessMs"), m_startupInteractiveMs);
    }
    return metrics;
}

qint64 MainWindow::elapsedSinceStartup() const {
    if (m_startupClock.isValid())
        return m_startupClock.elapsed();
    return m_startupElapsedOffsetMs + m_startupElapsed.elapsed();
}

void MainWindow::resetStartupPanelLoad(FilePanel *panel, quint64 generation) {
    if (m_startupInteractiveMs >= 0)
        return;
    const int panelIndex = panel == m_leftPanel ? 0 : panel == m_rightPanel ? 1 : -1;
    if (panelIndex < 0)
        return;

    m_startupPanelGeneration[panelIndex] = generation;
    m_startupPanelLoaded[panelIndex] = false;
    m_startupPanelInteractionScheduled[panelIndex] = false;
    m_startupPanelInteractive[panelIndex] = false;
    if (!m_startupPanelLoaded[0] || !m_startupPanelLoaded[1])
        m_startupPanelsLoadedMs = -1;
}

void MainWindow::markStartupPanelLoaded(FilePanel *panel, quint64 generation) {
    if (m_startupInteractiveMs >= 0)
        return;
    const int panelIndex = panel == m_leftPanel ? 0 : panel == m_rightPanel ? 1 : -1;
    if (panelIndex < 0 || generation != panel->model()->loadGeneration() ||
        generation != m_startupPanelGeneration[panelIndex])
        return;

    m_startupPanelLoaded[panelIndex] = true;
    if (m_startupPanelLoaded[0] && m_startupPanelLoaded[1])
        m_startupPanelsLoadedMs = elapsedSinceStartup();

    if (m_startupVisible) {
        m_startupPanelInteractionScheduled[panelIndex] = false;
        scheduleStartupPanelInteraction(panel);
    }
}

void MainWindow::scheduleStartupPanelInteraction(FilePanel *panel) {
    if (m_startupInteractiveMs >= 0)
        return;
    const int panelIndex = panel == m_leftPanel ? 0 : panel == m_rightPanel ? 1 : -1;
    if (!m_startupVisible || panelIndex < 0 || !m_startupPanelLoaded[panelIndex] ||
        m_startupPanelInteractionScheduled[panelIndex] || m_startupPanelInteractive[panelIndex])
        return;

    const quint64 generation = m_startupPanelGeneration[panelIndex];
    m_startupPanelInteractionScheduled[panelIndex] = true;
    QTimer::singleShot(0, this, [this, panel, panelIndex, generation] {
        if (m_startupInteractiveMs >= 0 || m_startupPanelInteractive[panelIndex] ||
            generation != m_startupPanelGeneration[panelIndex] ||
            generation != panel->model()->loadGeneration())
            return;
        FileListView *view = panel->view();
        if (!isVisible() || !panel->isVisible() || !view || !view->isVisible() ||
            panel->model()->rowCount() < 2) {
            m_startupPanelInteractionScheduled[panelIndex] = false;
            return;
        }

        const QModelIndex current = view->currentIndex();
        const QModelIndex target = panel->model()->index(current.isValid() && current.row() == 0 ? 1 : 0, 0);
        if (!current.isValid() || !target.isValid() || target == current) {
            m_startupPanelInteractionScheduled[panelIndex] = false;
            return;
        }
        view->setCurrentIndex(target);
        if (generation != m_startupPanelGeneration[panelIndex] ||
            generation != panel->model()->loadGeneration() || view->currentIndex() != target) {
            m_startupPanelInteractionScheduled[panelIndex] = false;
            return;
        }

        m_startupPanelInteractive[panelIndex] = true;
        if (m_startupPanelInteractive[0] && m_startupPanelInteractive[1] &&
            m_startupInteractiveMs < 0) {
            m_startupInteractiveMs = elapsedSinceStartup();
            emit startupReady();
        }
    });
}

// External-device hot-plug watcher, SMB neighbourhood browser, and the daily
// background update check. Kept out of the (already large) constructor body.
void MainWindow::setupFeatureBatch() {
    if (m_featureBatchStarted)
        return;
    m_featureBatchStarted = true;

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    // Removable-device hot-plug: when a new USB stick / phone / drive appears and
    // the preference is on, mount it and open it in a fresh, activated tab.
    m_deviceMonitor = new RemovableDeviceMonitor(this);
#endif

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || FILECOMMANDER_HAS_NETWORK
    // The folder trees organise themselves around devices and live connections.
    // Both panels share one registry so each can see (and grey out) the other's
    // connections; hot-plug and connect/disconnect drive the rebuilds, no polling.
#if FILECOMMANDER_HAS_NETWORK
    m_connRegistry = new NetworkTreeRegistry(this);
#endif
    m_leftPanel->setTreeSources(m_deviceMonitor, m_connRegistry);
    m_rightPanel->setTreeSources(m_deviceMonitor, m_connRegistry);
#endif

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    connect(m_deviceMonitor, &RemovableDeviceMonitor::deviceAdded, this,
            [this](const RemovableDevice &dev) {
                if (!m_settings.autoOpenNewDevice() || !m_activePanel)
                    return;
                QString mountPoint = dev.mountPoint;
                if (mountPoint.isEmpty())
                    mountPoint = m_deviceMonitor->ensureMounted(dev.id);
                if (mountPoint.isEmpty())
                    return;
                m_activePanel->newTab();
                m_activePanel->navigateTo(mountPoint);
            });

    // Seed the current removable-mount snapshot so the first devicesChanged()
    // diffs against reality, not an empty set.
    auto currentRemovableMounts = [this] {
        QStringList mounts;
        for (const RemovableDevice &dev : m_deviceMonitor->devices())
            if (dev.isMounted && !dev.mountPoint.isEmpty())
                mounts.append(dev.mountPoint);
        return mounts;
    };
    m_removableMounts = currentRemovableMounts();

    // When a removable volume is unmounted or unplugged, close any tab (in
    // either panel) that was browsing it. devicesChanged() covers both a device
    // vanishing outright and a still-plugged device merely being unmounted.
    connect(m_deviceMonitor, &RemovableDeviceMonitor::devicesChanged, this,
            [this, currentRemovableMounts] {
                const QStringList fresh = currentRemovableMounts();
                for (const QString &gone : m_removableMounts) {
                    if (fresh.contains(gone))
                        continue; // still mounted -- nothing to do
                    m_leftPanel->closeTabsOnMount(gone);
                    m_rightPanel->closeTabsOnMount(gone);
                }
                m_removableMounts = fresh;
            });

#endif

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
    // Warm the SMB-neighbourhood cache after the first frame's deferred feature
    // batch has started. The picker still starts/rescans on demand, but a quiet
    // background pass restores the pre-startup-optimization behaviour where
    // nearby devices are usually already listed when the user opens it.
    m_smbBrowser = new SmbHostBrowser(this);
    QTimer::singleShot(0, this, [browser = QPointer<SmbHostBrowser>(m_smbBrowser)] {
        if (browser)
            browser->startDiscovery(false);
    });
#endif

    // The first of the once-a-day update checks. The timer that repeats it is
    // armed in the constructor; this is only the one that would otherwise
    // compete with the first visible frame.
    maybeRunScheduledUpdateCheck();

    // Trim the thumbnail disk cache back under its limit, once, a few seconds
    // in. Deferred rather than immediate because it stats every stored file and
    // startup has better things to do with the disk; see
    // ThumbnailCache::scheduleMaintenance().
    ThumbnailCache::instance().scheduleMaintenance();

    // A computer view opened before this point is missing its removable-media
    // and network-neighbourhood sections, because the monitors that supply them
    // did not exist yet. They do now, so fill them in.
    refreshComputerViews();
}

void MainWindow::openFolders(const QStringList &folders) {
    for (int i = 0; i < folders.size(); ++i) {
        FilePanel *panel = i == 1 ? m_rightPanel : m_leftPanel;
        if (i >= 2)
            panel->newTab();
        panel->openLocalInTab(-1, folders.at(i));
    }
    if (!folders.isEmpty())
        setActivePanel(m_leftPanel);
}

void MainWindow::runPackageSmoke(const QString &directory) {
    const QDir dir(directory);
    if (!dir.exists() || !m_leftPanel) {
        QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(2); });
        return;
    }
    QuickView *const quickView = ensureQuickView();

    // The order deliberately crosses all runtime preview boundaries. showFile()
    // also works while the embedded pane is parked, which keeps this invisible
    // to users but still instantiates each backend and its helper process.
    QStringList files;
    for (const QString &name : {QStringLiteral("smoke.png"), QStringLiteral("smoke.pdf"),
                                QStringLiteral("smoke.docx"), QStringLiteral("smoke.zip"),
                                QStringLiteral("smoke.wav"), QStringLiteral("smoke.mp4")}) {
        if (dir.exists(name))
            files.append(name);
    }
    if (files.isEmpty()) {
        QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(2); });
        return;
    }
    m_leftPanel->navigateTo(dir.absolutePath());

    auto index = std::make_shared<int>(0);
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, quickView, dir, files, index, next] {
        if (*index >= files.size()) {
            QTimer::singleShot(1200, qApp, [] {
                // Package smoke has already exercised the preview stack. Return
                // success without running process-wide teardown, which can be
                // polluted by injected TSF/input-method DLLs on Windows.
                std::_Exit(0);
            });
            return;
        }
        quickView->showFile(dir.filePath(files.at((*index)++)));
        QTimer::singleShot(3500, this, *next);
    };
    (*next)();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // While the embedded preview is active, plain Tab/Backtab pressed with focus
    // inside the preview pane returns focus to the active file list (the reverse
    // of FilePanel::switchPanelRequested, which moves list -> preview). Cheap
    // early-outs keep this off the hot path for every other event.
    if (m_quickViewActive && event->type() == QEvent::KeyPress && m_quickView && m_activePanel) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) &&
            !(ke->modifiers() & Qt::ControlModifier)) {
            QWidget *fw = QApplication::focusWidget();
            if (fw && m_quickView->isAncestorOf(fw)) {
                m_activePanel->view()->setFocus(Qt::TabFocusReason);
                return true; // consume: preview -> list
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

namespace {
// Edges whose grab band (straddling the visible content rectangle's edge)
// contains p. `content` is the window rect inset by the shadow margin.
Qt::Edges edgesAt(const QRect &content, const QPoint &p) {
    Qt::Edges e;
    if (qAbs(p.x() - content.left()) <= kResizeGrab)
        e |= Qt::LeftEdge;
    else if (qAbs(p.x() - content.right()) <= kResizeGrab)
        e |= Qt::RightEdge;
    if (qAbs(p.y() - content.top()) <= kResizeGrab)
        e |= Qt::TopEdge;
    else if (qAbs(p.y() - content.bottom()) <= kResizeGrab)
        e |= Qt::BottomEdge;
    return e;
}

Qt::CursorShape cursorForEdges(Qt::Edges e) {
    const bool l = e & Qt::LeftEdge, r = e & Qt::RightEdge;
    const bool t = e & Qt::TopEdge, b = e & Qt::BottomEdge;
    if ((l && t) || (r && b))
        return Qt::SizeFDiagCursor;
    if ((r && t) || (l && b))
        return Qt::SizeBDiagCursor;
    if (l || r)
        return Qt::SizeHorCursor;
    if (t || b)
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}
} // namespace

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        return;
    }
    if (event->type() == QEvent::WindowStateChange) {
        if (m_titleBar)
            m_titleBar->syncWindowState();
        // A maximized window fills the screen: drop the shadow margin (and its
        // rounded corners) so no transparent gap shows at the screen edges.
        const int m = isMaximized() ? 0 : kShadowMargin;
        setContentsMargins(m, m, m, m);
        // State flips are single events (not a drag): apply the mask now so the
        // corners are correct the moment the window lands.
        if (m_maskTimer)
            m_maskTimer->stop();
        applyRoundedMask();
        updateOpaqueRegion(); // margin (hence opaque area) just changed
        update();
    }
}

bool MainWindow::event(QEvent *event) {
    // Frameless edge resize: the thin border around the content (not covered by
    // the central widget) reaches this handler. Show the resize cursor on hover
    // and hand off to the window manager on press.
    if (!isMaximized() &&
        (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress)) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QRect content = rect().adjusted(kShadowMargin, kShadowMargin,
                                              -kShadowMargin, -kShadowMargin);
        const Qt::Edges edges = edgesAt(content, me->pos());
        // Popup menus can replay their closing mouse event to this window even
        // when the pointer is over a title-bar child. The resize band overlaps
        // the first few pixels of that child, so coordinate-only hit testing
        // would leave the native cursor stuck as SizeVerCursor. Resize only in
        // the exposed shadow margin; normal child chrome owns its own cursor.
        const bool exposedResizeBand = childAt(me->pos()) == nullptr;
        if (event->type() == QEvent::MouseMove) {
            if (me->buttons() == Qt::NoButton) {
                // setCursor() on the window is inherited by every child that has
                // no cursor of its own, so off the edge we must UNSET it (not
                // force Arrow) — otherwise it overrides a child's own cursor
                // (e.g. the header's column-resize cursor).
                if (edges != Qt::Edges() && exposedResizeBand)
                    setCursor(cursorForEdges(edges));
                else
                    unsetCursor();
            }
        } else if (edges != Qt::Edges() && exposedResizeBand &&
                   me->button() == Qt::LeftButton) {
            if (QWindow *handle = windowHandle()) {
                handle->startSystemResize(edges);
                // The WM drives the cursor during the drag; drop our override so
                // it doesn't stay stuck as a resize shape (and get inherited by
                // every child) once the resize ends.
                unsetCursor();
                return true;
            }
        }
    }
    return QMainWindow::event(event);
}

void MainWindow::ensureFrameCache() {
    const QColor bg = palette().color(QPalette::Window);
    if (!m_frameCache.isNull() && m_frameCacheColor == bg)
        return;
    m_frameCacheColor = bg;

    // Render the shadow + rounded frame ONCE at the smallest size whose corners
    // and edges are fully representative; paintEvent then blits it 9-patch
    // style. The old path re-rasterized 17 anti-aliased rounded rects over the
    // whole window on every repaint, which made interactive resizing crawl.
    // Corner tiles must span the shadow margin + corner radius; one extra pixel
    // row/column in the middle stretches cleanly (all mid-frame rows are
    // identical).
    const int corner = kShadowMargin + kCornerRadius + 1;
    const int size = corner * 2 + 2;
    m_frameCache = QPixmap(size, size);
    m_frameCache.fill(Qt::transparent);

    QPainter p(&m_frameCache);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect content = QRect(0, 0, size, size)
                              .adjusted(kShadowMargin, kShadowMargin, -kShadowMargin,
                                        -kShadowMargin);
    p.setPen(Qt::NoPen);
    for (int i = kShadowMargin; i >= 1; --i) {
        const int alpha = 46 * (kShadowMargin - i + 1) / kShadowMargin;
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(QRectF(content).adjusted(-i, -i + 1, i, i + 1),
                          kCornerRadius + i, kCornerRadius + i);
    }
    p.setBrush(bg);
    p.drawRoundedRect(content, kCornerRadius, kCornerRadius);
}

void MainWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);

    // Maximized: fill the whole rect square (no shadow, no rounding).
    if (isMaximized() || contentsMargins().left() == 0) {
        p.fillRect(rect(), palette().color(QPalette::Window));
        scheduleMediaWarmupAfterFirstPaint();
        scheduleFeatureBatchAfterFirstPaint();
        return;
    }

    ensureFrameCache();
    // 9-patch blit: four corners at 1:1, edges stretched along one axis, the
    // centre stretched both ways (it's a solid fill, so stretching is exact).
    const int c = kShadowMargin + kCornerRadius + 1; // corner tile edge
    const int sw = m_frameCache.width();
    const int w = width(), h = height();
    const QPixmap &src = m_frameCache;

    // Corners.
    p.drawPixmap(0, 0, src, 0, 0, c, c);
    p.drawPixmap(w - c, 0, src, sw - c, 0, c, c);
    p.drawPixmap(0, h - c, src, 0, sw - c, c, c);
    p.drawPixmap(w - c, h - c, src, sw - c, sw - c, c, c);
    // Edges (stretched along their length).
    p.drawPixmap(QRect(c, 0, w - 2 * c, c), src, QRect(c, 0, sw - 2 * c, c));
    p.drawPixmap(QRect(c, h - c, w - 2 * c, c), src, QRect(c, sw - c, sw - 2 * c, c));
    p.drawPixmap(QRect(0, c, c, h - 2 * c), src, QRect(0, c, c, sw - 2 * c));
    p.drawPixmap(QRect(w - c, c, c, h - 2 * c), src, QRect(sw - c, c, c, sw - 2 * c));
    // Centre (solid window colour; fillRect is cheaper than a stretched blit).
    p.fillRect(QRect(c, c, w - 2 * c, h - 2 * c), m_frameCacheColor);
    scheduleMediaWarmupAfterFirstPaint();
    scheduleFeatureBatchAfterFirstPaint();
}

void MainWindow::scheduleMediaWarmupAfterFirstPaint() {
    if (!kAutomaticMediaWarmEnabled)
        return;
    if (!isVisible() || m_mediaWarmScheduled || m_mediaWarmComplete || !m_mediaWarmTimer)
        return;
    m_mediaWarmScheduled = true;
    m_mediaWarmTimer->start();
}

QuickView *MainWindow::ensureQuickView() {
    if (m_quickView)
        return m_quickView;

    m_quickView = new QuickView(m_settings, QuickView::Context::Embedded, this);
    m_quickView->hide(); // parked until Ctrl+Q swaps it into a panel slot
    m_quickView->setContentFontFamily(m_settings.globalFontFamily());
    m_quickView->setContentFontSize(m_settings.listFontSize());
    // Stop button on the remote-preview download page cancels the in-flight fetch.
    connect(m_quickView, &QuickView::downloadCancelRequested, this,
            &MainWindow::cancelPreviewDownload);
    // A streamed remote clip that wouldn't play falls back to the download path
    // by re-running the preview with streaming suppressed for that one file.
    connect(m_quickView, &QuickView::streamFailed, this, [this](const QString &) {
        if (!m_activePanel)
            return;
        m_streamFailedEntry = m_activePanel->currentEntryPath();
        updateQuickView();
    });
    m_mediaWarmTimer = new QTimer(this);
    m_mediaWarmTimer->setObjectName(QStringLiteral("mediaWarmTimer"));
    m_mediaWarmTimer->setSingleShot(true);
    m_mediaWarmTimer->setInterval(750);
    connect(m_mediaWarmTimer, &QTimer::timeout, m_quickView,
            &QuickView::warmMediaEngine);
    connect(m_quickView, &QuickView::mediaEngineWarmed, this,
            [this](qint64 elapsedMs) {
                m_mediaWarmComplete = true;
                if (m_mediaWarmTimer && m_mediaWarmTimer->isActive())
                    m_mediaWarmTimer->stop();
                qInfo() << "Media engine warm-up completed in" << elapsedMs << "ms";
            });
    connect(m_quickView, &QuickView::mediaEngineWarmFailed, this,
            [this](const QString &message) {
                m_mediaWarmComplete = true;
                if (m_mediaWarmTimer && m_mediaWarmTimer->isActive())
                    m_mediaWarmTimer->stop();
                qWarning() << "Media engine warm-up failed:" << message;
            });
    return m_quickView;
}

TransferProgressDialog *MainWindow::ensureTransferProgressDialog() {
    if (!m_progressDialog)
        m_progressDialog = new TransferProgressDialog(m_queue, this);
    return m_progressDialog;
}

void MainWindow::scheduleFeatureBatchAfterFirstPaint() {
    if (!isVisible() || m_featureBatchScheduled || m_featureBatchStarted)
        return;
    m_featureBatchScheduled = true;
    QTimer::singleShot(kStartupFeatureDelayMs, this, &MainWindow::setupFeatureBatch);
}

void MainWindow::applyRoundedMask() {
    QWidget *c = centralWidget();
    if (!c)
        return;
    // Maximized: no rounding, so drop any mask (square, full-bleed).
    if (isMaximized() || contentsMargins().left() == 0) {
        c->clearMask();
        return;
    }
    // Round only the bottom corners (the title bar covers the top ones); this
    // clips the bottom-most bar so the window's rounded silhouette shows.
    const int w = c->width(), h = c->height();
    const int r = kCornerRadius;
    QPainterPath path;
    path.moveTo(0, 0);
    path.lineTo(w, 0);
    path.lineTo(w, h - r);
    path.arcTo(w - 2 * r, h - 2 * r, 2 * r, 2 * r, 0, -90);
    path.lineTo(r, h);
    path.arcTo(0, h - 2 * r, 2 * r, 2 * r, 270, -90);
    path.closeSubpath();
    c->setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void MainWindow::updateOpaqueRegion() {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    // The property is an X11/NETWM hint; off X11 there's no connection to set it
    // on (QX11Info::connection() would be null), so bail.
    if (!QX11Info::isPlatformX11())
        return;
    QWindow *win = windowHandle();
    if (!win)
        return; // native window not created yet (before first show)
    xcb_connection_t *conn = QX11Info::connection();
    if (!conn)
        return;

    // Intern the atom once; its value is stable for the connection's lifetime.
    static xcb_atom_t opaqueAtom = XCB_ATOM_NONE;
    if (opaqueAtom == XCB_ATOM_NONE) {
        static const char kName[] = "_NET_WM_OPAQUE_REGION";
        xcb_intern_atom_cookie_t cookie =
            xcb_intern_atom(conn, 0, sizeof(kName) - 1, kName);
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, nullptr);
        if (!reply)
            return;
        opaqueAtom = reply->atom;
        free(reply);
    }

    // Opaque interior = visible content rect (window minus the translucent
    // shadow margin), minus the rounded corners. Maximized: the whole window is
    // opaque and square. The property is a flat list of (x, y, w, h) CARDINALs
    // in *device* pixels, so scale logical geometry by the device pixel ratio.
    const int m = (isMaximized() || contentsMargins().left() == 0) ? 0 : kShadowMargin;
    const int radius = (m == 0) ? 0 : kCornerRadius;
    const int x = m, y = m, w = width() - 2 * m, h = height() - 2 * m;
    if (w <= 0 || h <= 0)
        return;

    const qreal dpr = devicePixelRatioF();
    auto px = [dpr](int v) { return static_cast<uint32_t>(qRound(v * dpr)); };

    QVector<uint32_t> region;
    if (radius <= 0) {
        region = {px(x), px(y), px(w), px(h)};
    } else {
        // Two rectangles forming a "plus": a full-width band inset top/bottom by
        // the radius, plus a full-height band inset left/right. Their union is
        // the content rect with the four corner squares removed, so the
        // compositor keeps blending the (transparent, mask-clipped) corners
        // rather than smearing opaque pixels into them.
        region = {
            px(x),          px(y + radius), px(w),              px(h - 2 * radius),
            px(x + radius), px(y),          px(w - 2 * radius), px(h),
        };
    }

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE,
                        static_cast<xcb_window_t>(win->winId()), opaqueAtom,
                        XCB_ATOM_CARDINAL, 32,
                        static_cast<uint32_t>(region.size()), region.constData());
    xcb_flush(conn);
#endif
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (!m_startupVisible) {
        m_startupVisible = true;
        m_startupVisibleMs = elapsedSinceStartup();
        if (m_startupPanelLoaded[0] && m_startupPanelLoaded[1])
            m_startupPanelsLoadedMs = qMax(m_startupPanelsLoadedMs, m_startupVisibleMs);
        scheduleStartupPanelInteraction(m_leftPanel);
        scheduleStartupPanelInteraction(m_rightPanel);
    }
    // A window restored maximized only reports isMaximized() reliably once it's
    // mapped; the constructor's setContentsMargins ran before that and left the
    // shadow margin in place, showing a blank ring around a maximized window.
    // Reconcile the margin + corner mask with the real state now.
    const int m = isMaximized() ? 0 : kShadowMargin;
    if (contentsMargins().left() != m) {
        setContentsMargins(m, m, m, m);
        applyRoundedMask();
    }
    // The native X window now exists: publish the opaque region for the first
    // time. resizeEvent / changeEvent keep it in sync from here on.
    updateOpaqueRegion();
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    // Recomputing + applying an XShape mask per resize step is expensive and
    // forces extra full repaints, so during an interactive drag we defer it:
    // one mask update ~50ms after the last step. The corners are briefly square
    // mid-drag, which is imperceptible while the window is in motion.
    if (m_maskTimer)
        m_maskTimer->start();
    else
        applyRoundedMask();
    // The opaque region tracks the window size; one async xcb request per step
    // is cheap (no round-trip), so keep it exact rather than deferring it.
    updateOpaqueRegion();
}

void MainWindow::bindShortcut(const QString &id, const QString &label,
                               const QKeySequence &defaultSeq, std::function<void()> handler) {
    // Settings is consulted here rather than inside the registry: what key a
    // command currently has is this application's business, not the registry's.
    m_commands.bind(id, label, defaultSeq, m_settings.shortcut(id, defaultSeq),
                    std::move(handler));
}

void MainWindow::registerCommand(const QString &id, const QString &label,
                                  std::function<void()> handler) {
    m_commands.registerCommand(id, label, std::move(handler));
}

void MainWindow::runFunctionKey(int index) {
    if (index < 0 || index >= 6)
        return;
    m_commands.run(m_fkeyCommands[index]);
}

void MainWindow::updateFunctionKeyLabels() {
    for (int i = 0; i < 6; ++i) {
        const QString label = m_commands.label(m_fkeyCommands[i]);
        m_functionKeyBar->setLabel(i, QStringLiteral("F%1  %2").arg(3 + i).arg(label));
    }
}

QString MainWindow::pickCommandId(const QString &title, const QString &currentId) {
    // List every command by label so the user can pick a replacement.
    QList<QPair<QString, QString>> commands; // (label, id)
    for (const QString &commandId : m_commands.ids())
        commands.append({m_commands.label(commandId), commandId});
    std::sort(commands.begin(), commands.end(),
              [](const auto &a, const auto &b) { return a.first.localeAwareCompare(b.first) < 0; });

    // FramelessDialog, not a bare QDialog: it wears the same self-drawn themed
    // title bar as the rest of the app, and drops the native one -- along with
    // the "?" context-help button Windows puts on a plain dialog.
    FramelessDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(420, 480);

    // Two columns: function name (left) and its shortcut (right-aligned).
    auto *tree = new QTreeWidget(&dlg);
    tree->setColumnCount(2);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    tree->setIndentation(0);
    tree->setUniformRowHeights(true);
    // Row/hover/selection colours come from the theme sheet's QTreeView rules.
    // They used to be set here from tree->palette(), which looked right only as
    // long as the palette carried the theme -- the themes style views through
    // the stylesheet instead, so the palette still held Qt's defaults and this
    // dialog kept painting a stock blue selection in every theme.
    QTreeWidgetItem *currentItem = nullptr;
    for (const auto &c : commands) {
        const QString &id = c.second;
        QKeySequence key = m_commands.sequence(id);
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, c.first);
        // Trailing spaces keep the right-aligned key clear of the (overlay)
        // scrollbar on the right.
        item->setText(1, key.toString(QKeySequence::NativeText) + QStringLiteral("     "));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, Qt::UserRole, id);
        if (id == currentId)
            currentItem = item;
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    if (currentItem)
        tree->setCurrentItem(currentItem);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    ttc::localizeStandardButtons(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr("Choose a function:"), &dlg));
    layout->addWidget(tree);
    layout->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted && tree->currentItem())
        return tree->currentItem()->data(0, Qt::UserRole).toString();
    return QString();
}

void MainWindow::changeFunctionKey(int index) {
    if (index < 0 || index >= 6)
        return;
    const QString id =
        pickCommandId(tr("Change F%1 Function").arg(3 + index), m_fkeyCommands[index]);
    if (!id.isEmpty()) {
        m_fkeyCommands[index] = id;
        m_settings.setFunctionKeyCommand(index, id);
        updateFunctionKeyLabels();
    }
}

void MainWindow::changeExtraKey(const QString &slot) {
    const bool leading = (slot == QLatin1String("leading"));
    const QString title =
        leading ? tr("Change Leading Button Function") : tr("Change Trailing Button Function");
    const QString current = leading ? m_leadingCommand : m_trailingCommand;
    const QString id = pickCommandId(title, current);
    if (id.isEmpty())
        return;
    if (leading)
        m_leadingCommand = id;
    else
        m_trailingCommand = id;
    m_settings.setExtraKeyCommand(slot, id);
    updateExtraKeyButtons();
}

void MainWindow::runExtraKey(const QString &slot) {
    const QString cmd = (slot == QLatin1String("leading")) ? m_leadingCommand : m_trailingCommand;
    m_commands.run(cmd);
}

void MainWindow::updateExtraKeyButtons() {
    m_leadingCommand = m_settings.extraKeyCommand("leading", "external-connect");
    m_trailingCommand = m_settings.extraKeyCommand("trailing", "notepad");
    m_functionKeyBar->setLeadingIcon(
        IconCache::instance().glyphIcon(QStringLiteral(":/icons/ext-connect.svg")));
    m_functionKeyBar->setTrailingIcon(
        IconCache::instance().glyphIcon(QStringLiteral(":/icons/notepad.svg")));
    m_functionKeyBar->setLeadingToolTip(m_commands.label(m_leadingCommand));
    m_functionKeyBar->setTrailingToolTip(
        m_commands.label(m_trailingCommand));
}

namespace {
#if FILECOMMANDER_HAS_NETWORK
// A native backend built for a saved bookmark: the (UNCONNECTED) provider plus
// the connect closure to run on the session worker thread. Empty provider means
// the protocol isn't a native backend.
struct SavedNativeProvider {
    std::shared_ptr<FileProvider> provider;
    std::function<bool(QString *)> connectFn;
    // Rebuilds the connect closure with user-entered credentials, so an anonymous
    // or wrong-password connection can be retried after a prompt.
    FileSystemModel::AuthRetryFactory authFactory;
};

// Builds (but does not connect) the native provider for a saved bookmark,
// mirroring ConnectDialog. The connect runs later on the worker thread via
// FileSystemModel::connectNetwork, so reopening a bookmark never blocks the UI.
// The password is pulled from the keyring by id.
SavedNativeProvider providerForSaved(const SavedConnection &c) {
    const auto protocol = static_cast<ConnectionProtocol>(c.protocol);
    const QString password = c.anonymous ? QString() : ConnectionStore::loadPassword(c.id);
    switch (protocol) {
    case ConnectionProtocol::Sftp: {
        auto p = std::make_shared<SftpProvider>();
        auto factory = [p, c](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>(
                [p, c, u, pw](QString *e) { return p->connectToHost(c.host, c.port, u, pw, e); });
        };
        return {p,
                [p, c, password](QString *e) {
                    return p->connectToHost(c.host, c.port, c.user, password, e);
                },
                factory};
    }
    case ConnectionProtocol::Ftp: {
        auto p = std::make_shared<CurlFtpProvider>();
        auto factory = [p, c](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>(
                [p, c, u, pw](QString *e) { return p->connectToHost(c.host, c.port, u, pw, e); });
        };
        return {p,
                [p, c, password](QString *e) {
                    return p->connectToHost(c.host, c.port, c.user, password, e);
                },
                factory};
    }
    case ConnectionProtocol::WebDav:
    case ConnectionProtocol::WebDavs: {
        auto p = std::make_shared<CurlWebDavProvider>();
        const bool useHttps = protocol == ConnectionProtocol::WebDavs;
        auto factory = [p, c, useHttps](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>([p, c, u, pw, useHttps](QString *e) {
                return p->connectToHost(c.host, c.port, u, pw, useHttps, e);
            });
        };
        return {p,
                [p, c, password, useHttps](QString *e) {
                    return p->connectToHost(c.host, c.port, c.user, password, useHttps, e);
                },
                factory};
    }
    case ConnectionProtocol::Smb: {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
        auto p = std::make_shared<SmbProvider>();
#elif defined(Q_OS_WIN)
        auto p = std::make_shared<WindowsSmbProvider>();
#else
        return {};
#endif
        auto factory = [p, c](const QString &u, const QString &pw) {
            return std::function<bool(QString *)>([p, c, u, pw](QString *e) {
                return p->connectToHost(c.host, u, pw, QString(), /*anonymous=*/false, e);
            });
        };
        return {p,
                [p, c, password](QString *e) {
                    return p->connectToHost(c.host, c.user, password, QString(), c.anonymous, e);
                },
                factory};
    }
    default:
        return {};
    }
}
#endif
} // namespace

void MainWindow::openExternalConnections() {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
    if (!m_activePanel)
        return;
    setupFeatureBatch();
    // A floating fly-out anchored above the launching (leading) button rather
    // than a modal dialog: it aggregates removable devices, saved bookmarks and
    // discovered SMB hosts; activating a row emits one of three signals which we
    // turn into a navigation in the active panel. The panel is non-modal and
    // deletes itself when it closes (WA_DeleteOnClose).
    auto *dlg = new ExternalConnectDialog(m_deviceMonitor, m_smbBrowser, this);

    connect(dlg, &ExternalConnectDialog::openLocalPath, this, [this](const QString &path) {
        if (!path.isEmpty())
            m_activePanel->navigateTo(path);
    });
    connect(dlg, &ExternalConnectDialog::openSavedConnection, this,
            &MainWindow::openSavedConnection);
    connect(dlg, &ExternalConnectDialog::openSmbHost, this, &MainWindow::browseSmbHost);
    // Manager button next to the "Saved Connections" header: open the connection
    // manager (add/edit/delete saved bookmarks; manual connect lives here too).
    connect(dlg, &ExternalConnectDialog::openConnectionManager, this,
            [this] { openServerConnectDialog(false); });

    // Pop up directly above the leading function-key button that launched it.
    dlg->popUpAbove(m_functionKeyBar->leadingButtonGlobalRect());
#elif FILECOMMANDER_HAS_NETWORK
    openServerConnectDialog(false);
#else
    ttc::information(this, tr("External Connections"),
                     tr("Network and removable-device connections are not enabled in this build."));
#endif
}

void MainWindow::openSavedConnection(const SavedConnection &conn) {
#if FILECOMMANDER_HAS_NETWORK
    auto native = providerForSaved(conn);
    if (!native.provider) {
        ttc::critical(this, tr("Connection Failed"), tr("Unsupported connection type."));
        return;
    }
    // Connect asynchronously on a worker thread; the status line shows
    // "connecting / reconnecting / failed". Never blocks the UI.
    const QString path = conn.remotePath.isEmpty() ? QStringLiteral("/") : conn.remotePath;
    // Open in a fresh tab on the left panel, not the active tab.
    FilePanel *panel = beginServerConnection();
    panel->model()->connectNetwork(native.provider, native.connectFn, path);
    // Show the host name on the tab immediately, before it connects.
    const QString label =
        conn.user.isEmpty() ? conn.host : conn.user + QLatin1Char('@') + conn.host;
    panel->setConnectingLabel(label, native.provider->scheme());
    // Wire the credential-retry factory so a wrong/missing keyring password
    // surfaces a login prompt AND the re-entered password actually reconnects --
    // without this, provideCredentials had no factory and silently discarded
    // what the user typed.
    if (native.authFactory)
        panel->model()->setAuthContext(label, native.authFactory);
    // Record the connection so it is re-established (with its label) on next
    // launch, and store the remote path we're opening.
    SavedConnection persist = conn;
    persist.remotePath = path;
    panel->setActiveTabConnInfo(persist);
    panel->navigateTo(path);
#else
    Q_UNUSED(conn);
    ttc::information(this, tr("External Connections"),
                     tr("Network and removable-device connections are not enabled in this build."));
#endif
}

void MainWindow::browseSmbHost(const QString &hostName) {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
    if (hostName.isEmpty())
        return;
    // Browse the host's shares anonymously; "/" lists the shares available.
    // Connect asynchronously so an unreachable host never freezes the UI.
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    auto provider = std::make_shared<SmbProvider>();
#elif defined(Q_OS_WIN)
    auto provider = std::make_shared<WindowsSmbProvider>();
#endif
    auto connectFn = [provider, hostName](QString *e) {
        return provider->connectToHost(hostName, QString(), QString(), QString(), true, e);
    };
    // Open in a fresh tab on the left panel, not the active tab.
    FilePanel *panel = beginServerConnection();
    panel->model()->connectNetwork(provider, connectFn, QStringLiteral("/"));
    // Show the host name on the tab immediately, before it connects.
    panel->setConnectingLabel(hostName, QStringLiteral("smb"));
    // Record for session reconnect (anonymous SMB browse of this host).
    SavedConnection smbInfo;
    smbInfo.protocol = static_cast<int>(ConnectionProtocol::Smb);
    smbInfo.host = hostName;
    smbInfo.anonymous = true;
    smbInfo.remotePath = QStringLiteral("/");
    panel->setActiveTabConnInfo(smbInfo);
    // If the anonymous browse is denied, prompt for a login and retry with it.
    panel->model()->setAuthContext(
        hostName, [provider, hostName](const QString &u, const QString &p) {
            return std::function<bool(QString *)>([provider, hostName, u, p](QString *e) {
                return provider->connectToHost(hostName, u, p, QString(), /*anonymous=*/false, e);
            });
        });
    panel->navigateTo(QStringLiteral("/"));
#else
    Q_UNUSED(hostName);
    ttc::information(this, tr("External Connections"),
                     tr("Network and removable-device connections are not enabled in this build."));
#endif
}

// Fetch each drive's icon on a worker and repaint when they are in. The query
// goes to the volume itself, so doing it lazily from FileSystemModel::data()
// meant doing it while PAINTING -- and a disconnected mapped drive can hold
// that up for seconds. Fire and forget: the icons are cached, so a run that
// outlives the panel has simply warmed the cache for the next one.
void MainWindow::warmDriveIcons(FilePanel *panel) {
    QStringList roots;
    for (const ComputerEntry &drive : ComputerCatalog::drives())
        roots << drive.target;
    if (roots.isEmpty())
        return;
    QPointer<FilePanel> guard(panel);
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [watcher, guard]() {
        watcher->deleteLater();
        if (guard && guard->view())
            guard->view()->viewport()->update();
    });
    watcher->setFuture(QtConcurrent::run([roots]() {
        for (const QString &root : roots)
            IconCache::instance().warmSystemIconForPath(root);
    }));
}

QVector<ComputerEntry> MainWindow::computerEntries() {
    // Deliberately does NOT force setupFeatureBatch(). Drives, user folders and
    // saved bookmarks need nothing but QStorageInfo, QStandardPaths and the
    // bookmark store, so the view can be built during startup without dragging
    // the device monitor and the SMB browser in front of the first paint --
    // which they are deliberately deferred past. The two sections that do need
    // them are simply absent until they exist, and setupFeatureBatch refreshes
    // any open view once it has them.

    // Removable media first, because the drive list is filtered against it: on
    // Windows a plugged-in stick is also a drive letter, and listing it in both
    // sections would have the user eject a device from one row and navigate into
    // a stale copy of it from another.
    QVector<ComputerEntry> removable;
    QSet<QString> removableRoots;
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    if (m_deviceMonitor) {
        for (const RemovableDevice &device : m_deviceMonitor->devices()) {
            ComputerEntry entry;
            entry.kind = ComputerEntry::Kind::RemovableDevice;
            entry.name = device.name;
            // The monitor's id, not the mount point: an unmounted device has no
            // mount point yet, and mounting it is exactly what activating the
            // row does.
            entry.target = device.id;
            entry.iconPath = QStringLiteral(":/icons/%1.svg").arg(device.iconName);
            removable.append(entry);
            if (!device.mountPoint.isEmpty())
                removableRoots.insert(QDir::fromNativeSeparators(device.mountPoint));
        }
    }
#endif

    QVector<ComputerEntry> entries;
    for (const ComputerEntry &drive : ComputerCatalog::drives()) {
        QString root = drive.target;
        while (root.size() > 1 && root.endsWith(QLatin1Char('/')))
            root.chop(1); // "C:/" and "C:" name the same volume as a mount point
        if (removableRoots.contains(drive.target) || removableRoots.contains(root))
            continue;
        entries.append(drive);
    }
    entries += ComputerCatalog::userFolders();
    entries += removable;
    entries += ComputerCatalog::savedServers();
    // Discovered SMB hosts are deliberately NOT listed here. Filling this view
    // would mean running a network scan every time it opens, for results that
    // are a browse of the neighbourhood rather than a place on this machine;
    // the connect fly-out is where that belongs and still offers it. What is
    // left is what the view is for: this computer's disks, whatever is plugged
    // into it, its user folders, and the servers already bookmarked.
    return entries;
}

void MainWindow::showComputerView(FilePanel *panel) {
    if (!panel)
        return;

#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    // Only once the window is up. During construction this would pull the
    // device monitor in ahead of the first paint, which the startup path exists
    // to avoid; setupFeatureBatch refreshes the view itself when it later runs.
    if (isVisible())
        setupFeatureBatch();
    if (m_deviceMonitor)
        connect(m_deviceMonitor, &RemovableDeviceMonitor::devicesChanged, this,
                &MainWindow::refreshComputerViews, Qt::UniqueConnection);
#endif
    panel->showComputer(computerEntries());
    warmDriveIcons(panel);
}

void MainWindow::refreshComputerViews() {
    const QVector<ComputerEntry> entries = computerEntries();
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        // showComputer() on a panel that is not in the view would ENTER it, so
        // an unrelated hot-plug would yank the listing out from under the user.
        if (panel && panel->isComputerView()) {
            panel->showComputer(entries);
            warmDriveIcons(panel);
        }
    }
}

void MainWindow::openComputerEntry(FilePanel *panel, const ComputerEntry &entry) {
    if (!panel || entry.target.isEmpty())
        return;

    switch (entry.kind) {
    case ComputerEntry::Kind::Drive:
    case ComputerEntry::Kind::UserFolder:
        // navigateTo() steps out of the computer view itself. Doing it here as
        // well would hide the transition from it, and with it the history entry
        // that lets Back return to the view.
        panel->navigateTo(entry.target);
        break;
    case ComputerEntry::Kind::RemovableDevice: {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
        if (!m_deviceMonitor)
            return;
        QString mountPoint;
        for (const RemovableDevice &device : m_deviceMonitor->devices()) {
            if (device.id == entry.target)
                mountPoint = device.mountPoint;
        }
        if (mountPoint.isEmpty()) {
            // Mount on demand; this can block briefly, hence the wait cursor.
            QString error;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            mountPoint = m_deviceMonitor->ensureMounted(entry.target, &error);
            QApplication::restoreOverrideCursor();
            if (mountPoint.isEmpty()) {
                // Stay in the computer view: the user can pick another row, and
                // dropping them somewhere else would hide what just failed.
                ttc::critical(this, tr("Mount Failed"),
                              tr("Could not mount the device.\n\n%1").arg(error));
                return;
            }
        }
        panel->navigateTo(mountPoint); // steps out of the computer view itself
#endif
        break;
    }
    case ComputerEntry::Kind::SavedServer: {
        const SavedConnection conn = ConnectionStore::load(entry.target);
        if (conn.id.isEmpty())
            return; // deleted between the listing being built and the click
        // Deliberately left in the computer view: the connection opens in a
        // fresh tab on the left panel (as it does from the connect fly-out), so
        // this panel's listing is not the one being replaced.
        openSavedConnection(conn);
        break;
    }
    case ComputerEntry::Kind::NetworkHost:
        browseSmbHost(entry.target);
        break;
    }
}

void MainWindow::toggleNotepad() {
    // A floating fly-out anchored above the trailing function-key button that
    // launched it (mirrors the external-connection panel), rather than a docked
    // third column. Non-modal; it auto-saves and deletes itself on close.
    auto *pad = new NotepadPanel(m_settings, this);
    // The app window's VISIBLE content rect in global coords: contentsRect()
    // excludes the frameless shadow margin, so the popup aligns to the real
    // window edges, not the shadow.
    const QRect appContent(mapToGlobal(contentsRect().topLeft()), contentsRect().size());
    pad->popUpAbove(m_functionKeyBar->trailingButtonGlobalRect(), appContent);
}

void MainWindow::showAboutDialog() {
    AboutDialog dlg(windowIcon(), this);
    dlg.exec();
}

bool MainWindow::updateCheckIsDue() const {
    return m_settings.autoUpdateCheck()
           && m_settings.updateLastCheckDate() != QDate::currentDate().toString(Qt::ISODate);
}

void MainWindow::maybeRunScheduledUpdateCheck() {
    if (!updateCheckIsDue())
        return;
    // Quiet on purpose: a release found this way only lights the title-bar
    // badge. Interrupting somebody mid-task with a modal dialog they did not
    // ask for is what makes people turn auto-update off.
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    auto *checker = new UpdateChecker(this);
    auto stamp = [this, today] { m_settings.setUpdateLastCheckDate(today); };
    connect(checker, &UpdateChecker::updateAvailable, this,
            [this, checker, stamp](const UpdateInfo &info) {
                m_pendingUpdate = info;
                m_hasUpdate = true;
                if (m_titleBar)
                    m_titleBar->setUpdateAvailable(true);
                stamp();
                checker->deleteLater();
            });
    connect(checker, &UpdateChecker::noUpdate, this, [checker, stamp] {
        stamp();
        checker->deleteLater();
    });
    // Deliberately NOT stamped: a check that failed did not happen, and a
    // machine that was offline this morning should try again this afternoon
    // rather than write the day off.
    connect(checker, &UpdateChecker::checkFailed, this,
            [checker](const QString &) { checker->deleteLater(); });
    checker->checkForUpdates();
}

void MainWindow::checkForUpdatesNow() {
    // A manual check: report the outcome directly (unlike the silent daily
    // check, which only lights the title-bar badge).
    auto *checker = new UpdateChecker(this);
    connect(checker, &UpdateChecker::updateAvailable, this, [this, checker](const UpdateInfo &info) {
        m_pendingUpdate = info;
        m_hasUpdate = true;
        if (m_titleBar)
            m_titleBar->setUpdateAvailable(true);
        m_settings.setUpdateLastCheckDate(QDate::currentDate().toString(Qt::ISODate));
        checker->deleteLater();
        showUpdateDialog();
    });
    connect(checker, &UpdateChecker::noUpdate, this, [this, checker] {
        m_settings.setUpdateLastCheckDate(QDate::currentDate().toString(Qt::ISODate));
        checker->deleteLater();
        ttc::information(this, tr("Check for Updates"),
                         tr("You are running the latest version."));
    });
    connect(checker, &UpdateChecker::checkFailed, this, [this, checker](const QString &err) {
        checker->deleteLater();
        ttc::warning(this, tr("Check for Updates"),
                     tr("Could not check for updates.\n\n%1").arg(err));
    });
    checker->checkForUpdates();
}

void MainWindow::showUpdateDialog() {
    if (!m_hasUpdate)
        return;
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    // Announce-only: the dialog hands over the address and the checksum, and
    // the user installs from the Microsoft Store or from the package
    // themselves. Nothing here downloads, replaces or restarts anything.
    UpdateDialog dlg(m_pendingUpdate, this);
    dlg.exec();
#else
    ttc::information(this, tr("Update Available"),
                     tr("Version %1 is available.\n\n%2\n\nDownload: %3")
                         .arg(m_pendingUpdate.version, m_pendingUpdate.notes,
                              m_pendingUpdate.url));
#endif
}

void MainWindow::showShortcutMenu(FilePanel *panel, const QPoint &globalPos) {
    QScopedPointer<QMenu> menu(buildShortcutMenu(panel));
    QPoint pos = globalPos;
    if (panel) {
        const int menuWidth = menu->sizeHint().width();
        const int rightEdge = panel->mapToGlobal(QPoint(panel->width(), 0)).x();
        pos.setX(rightEdge - menuWidth);
    }
    menu->exec(pos);
}

QMenu *MainWindow::buildShortcutMenu(FilePanel *panel) {
    auto *menu = new QMenu(this);
    // Every command here is scoped to the panel whose "✳" was clicked, not to
    // whichever panel happens to be active when the item is chosen. The button
    // does emit panelActivated() before opening the menu, but closing a popup
    // restores keyboard focus to the widget that had it beforehand -- the OTHER
    // panel's view -- and that focus-in makes it active again, all before the
    // action's triggered() is delivered. So re-assert the owning panel here,
    // where it cannot be undone underneath us.
    auto addCommand = [&](const QString &id, const QString &label) {
        std::function<void()> handler = m_commands.handler(id);
        addCommandAction(menu, id, label, [this, panel, handler] {
            if (panel) {
                setActivePanel(panel);
                // ... and take the keyboard with it, or the highlight would say
                // one panel while the arrow keys drove the other.
                if (QWidget *view = panel->activeView())
                    view->setFocus();
            }
            if (handler)
                handler();
        });
    };

    addCommand(QStringLiteral("quickView"), tr("Open Quick Preview"));
    addCommand(QStringLiteral("calcSize"), tr("Calculate Folder Size"));
    addCommand(QStringLiteral("toggleViewMode"),
               panel && panel->isThumbnailMode() ? tr("Switch to List View")
                                                 : tr("Switch to Thumbnail View"));
    addCommand(QStringLiteral("toggleHidden"), tr("Show Hidden Files"));
    addCommand(QStringLiteral("syncDirectories"), tr("Synchronize Directories"));
    addCommand(QStringLiteral("compareDirectories"), tr("Compare Directories"));
    addCommand(QStringLiteral("search"), tr("Find Files"));
    addCommand(QStringLiteral("quickFilter"), tr("Filter Files"));
    addCommand(QStringLiteral("selectPattern"), tr("Select by Pattern"));
    addCommand(QStringLiteral("invertSelection"), tr("Invert Selection"));
    addCommand(QStringLiteral("undo"), tr("Undo Previous Operation"));
    addCommand(QStringLiteral("openTerminal"), tr("Open Terminal Here"));
    return menu;
}

void MainWindow::setupShortcuts() {
    // The F3-F8 defaults are reassignable slots (see below), so register them
    // as commands without a fixed key.
    registerCommand("view", tr("View"), [this] { viewCurrent(); });
    registerCommand("edit", tr("Edit"), [this] { editCurrent(); });
    registerCommand("copy", tr("Copy"), [this] { copySelected(); });
    registerCommand("move", tr("Move"), [this] { moveSelected(); });
    registerCommand("mkdir", tr("New Folder"), [this] { makeDirectory(); });
    registerCommand("delete", tr("Delete (to trash)"), [this] { deleteSelected(false); });
    registerCommand("external-connect", tr("Connect External / Devices"),
                    [this] { openExternalConnections(); });
    registerCommand("notepad", tr("Quick Notepad"), [this] { toggleNotepad(); });
    bindShortcut("notepad", tr("Quick Notepad"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N), [this] { toggleNotepad(); });
    bindShortcut("checksums", tr("Calculate Checksums"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_H), [this] { calculateChecksums(); });
    bindShortcut("secureWipe", tr("Secure Wipe"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_W), [this] { secureWipeSelected(); });
    bindShortcut("compareFiles", tr("Compare Files"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V), [this] { compareSelectedFiles(); });
    bindShortcut("keyboardShortcuts", tr("Keyboard Shortcuts"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_K), [this] { openShortcutsDialog(); });
    // Opens the server connection window, not the fly-out over the function-key
    // bar. That fly-out is a launcher for things already set up (devices,
    // saved bookmarks, discovered hosts); this entry is for managing the
    // connections themselves, which is what its name promises.
    bindShortcut("connectionManager", tr("Manage Network Connections"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_O),
                 [this] { openServerConnectDialog(false); });
    bindShortcut("chooseFont", tr("Choose Font"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F),
                 [this] { chooseGlobalFont(); });
    bindShortcut("increaseFontSize", tr("Increase Font Size"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Equal), [this] {
                     const int size = qMin(16, m_settings.listFontSize() + 1);
                     m_settings.setListFontSize(size);
                     m_leftPanel->setListFontSize(size);
                     m_rightPanel->setListFontSize(size);
                     if (m_quickView)
                         m_quickView->setContentFontSize(size);
                 });
    bindShortcut("decreaseFontSize", tr("Decrease Font Size"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Minus), [this] {
                     const int size = qMax(8, m_settings.listFontSize() - 1);
                     m_settings.setListFontSize(size);
                     m_leftPanel->setListFontSize(size);
                     m_rightPanel->setListFontSize(size);
                     if (m_quickView)
                         m_quickView->setContentFontSize(size);
                 });
    bindShortcut("cycleTheme", tr("Cycle Theme"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T),
                 [this] {
                     const int next = (static_cast<int>(m_settings.theme()) + 1) % 4;
                     setTheme(static_cast<Settings::Theme>(next));
                 });
    bindShortcut("toggleFunctionKeyBar", tr("Show Function Key Bar"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J), [this] {
                     const bool visible = m_functionKeyBar->isHidden();
                     m_functionKeyBar->setVisible(visible);
                     m_settings.setShowFunctionKeyBar(visible);
                 });
    bindShortcut("toggleCommandBar", tr("Show Command Bar"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C), [this] {
                     const bool visible = m_commandBar->isHidden();
                     m_commandBar->setVisible(visible);
                     m_settings.setShowCommandBar(visible);
                 });
    bindShortcut("toggleTabBar", tr("Show File Tab Bar"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B), [this] {
                     const bool visible = !m_settings.showTabBar();
                     m_leftPanel->setTabBarVisible(visible);
                     m_rightPanel->setTabBarVisible(visible);
                     m_settings.setShowTabBar(visible);
                 });
    bindShortcut("toggleShortcutLabels", tr("Display Shortcut Labels"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), [this] {
                     m_settings.setShowShortcutLabels(!m_settings.showShortcutLabels());
                 });
    bindShortcut("toggleDirectArchives", tr("Open Archives as Folders"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A), [this] {
                     const bool direct = m_settings.archiveAsFolder();
                     m_settings.setArchiveAsFolder(!direct);
                     m_leftPanel->setArchiveAsFolder(!direct);
                     m_rightPanel->setArchiveAsFolder(!direct);
                 });
    bindShortcut("toggleDeleteConfirmation", tr("Skip Trash Delete Confirmation"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_D), [this] {
                     m_settings.setConfirmDelete(!m_settings.confirmDelete());
                 });
    bindShortcut("toggleAutoUpdate", tr("Automatic Update Check"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_U), [this] {
                     m_settings.setAutoUpdateCheck(!m_settings.autoUpdateCheck());
                 });
    bindShortcut("openTerminal", tr("Open Terminal Here"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Return), [this] { openTerminalHere(); });
    bindShortcut("syncDirectories", tr("Synchronize Directories"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Y), [this] { openSyncDialog(); });
    bindShortcut("compareDirectories", tr("Compare Directories"),
                 QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_I), [this] { compareDirectories(); });
    bindShortcut("selectPattern", tr("Select by Pattern"), QKeySequence(Qt::CTRL | Qt::Key_Plus),
                 [this] { if (m_activePanel) m_activePanel->selectByPattern(true); });
    bindShortcut("invertSelection", tr("Invert Selection"),
                 QKeySequence(Qt::CTRL | Qt::Key_Asterisk),
                 [this] { if (m_activePanel) m_activePanel->invertSelection(); });
    bindShortcut("deletePermanent", tr("Delete Permanently"),
                 QKeySequence(Qt::SHIFT | Qt::Key_Delete), [this] { deleteSelected(true); });
    bindShortcut("deleteAlt", tr("Delete (Del key)"), QKeySequence(Qt::Key_Delete),
                 [this] { deleteSelected(false); });
    bindShortcut("rename", tr("Rename"), QKeySequence(Qt::Key_F2), [this] { renameCurrent(); });

    bindShortcut("newTab", tr("New Tab"), QKeySequence(Qt::CTRL | Qt::Key_T), [this] {
        if (m_activePanel)
            m_activePanel->newTab();
    });
    bindShortcut("closeTab", tr("Close Tab"), QKeySequence(Qt::CTRL | Qt::Key_W), [this] {
        if (m_activePanel)
            m_activePanel->closeCurrentTab();
    });
    bindShortcut("nextTab", tr("Next Tab"), QKeySequence(Qt::CTRL | Qt::Key_Tab), [this] {
        if (m_activePanel)
            m_activePanel->nextTab();
    });
    bindShortcut("prevTab", tr("Previous Tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
                 [this] {
                     if (m_activePanel)
                         m_activePanel->prevTab();
                 });

    bindShortcut("search", tr("Search Files"), QKeySequence(Qt::CTRL | Qt::Key_F),
                 [this] { openSearch(); });
    // Focus the bottom command line (TC-style). Reveals it first if the user has
    // hidden it via the View menu, so the command bar is always keyboard-reachable
    // -- previously CommandBar::focusInput() was dead code with no way to reach it.
    bindShortcut("focusCommandBar", tr("Command Line"), QKeySequence(Qt::CTRL | Qt::Key_E),
                 [this] {
                     if (!m_commandBar)
                         return;
                     if (m_commandBar->isHidden()) {
                         m_commandBar->setVisible(true);
                         m_settings.setShowCommandBar(true);
                     }
                     // Show the active panel's directory before focusing (in case a
                     // panel switch happened while the bar was hidden).
                     if (m_activePanel)
                         m_commandBar->setDirectory(m_activePanel->currentPath());
                     m_commandBar->focusInput();
                 });
    bindShortcut("compress", tr("Compress Selected"), QKeySequence(Qt::ALT | Qt::Key_F5),
                 [this] { compressSelected(); });
    bindShortcut("copyPath", tr("Copy Path"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                 [this] {
                     if (m_activePanel) {
                         const QStringList paths = m_activePanel->selectedPaths();
                         if (!paths.isEmpty())
                             QGuiApplication::clipboard()->setText(paths.join('\n'));
                     }
                 });
    bindShortcut("refresh", tr("Refresh"), QKeySequence(Qt::CTRL | Qt::Key_R),
                 [this] { refreshActivePanel(); });
    bindShortcut("exit", tr("Exit"), QKeySequence(Qt::Key_F10), [this] { close(); });

    bindShortcut("cutClipboard", tr("Cut"), QKeySequence(Qt::CTRL | Qt::Key_X),
                 [this] { cutSelectionToClipboard(); });
    bindShortcut("copyClipboard", tr("Copy to Clipboard"), QKeySequence(Qt::CTRL | Qt::Key_C),
                 [this] { copySelectionToClipboard(); });
    bindShortcut("pasteClipboard", tr("Paste"), QKeySequence(Qt::CTRL | Qt::Key_V),
                 [this] { pasteFromClipboard(); });
    bindShortcut("multiRename", tr("Multi-Rename Tool"), QKeySequence(Qt::CTRL | Qt::Key_M),
                 [this] { openMultiRenameDialog(); });
    bindShortcut("directoryHotlist", tr("Directory Hotlist"), QKeySequence(Qt::CTRL | Qt::Key_D),
                 [this] { openDirectoryHotlist(); });
    bindShortcut("quickFilter", tr("Quick Filter"), QKeySequence(Qt::CTRL | Qt::Key_S), [this] {
        if (m_activePanel)
            m_activePanel->showQuickFilter();
    });
    bindShortcut("properties", tr("Properties"), QKeySequence(Qt::Key_F9),
                 [this] { showProperties(); });
    bindShortcut("toggleHidden", tr("Show Hidden Files"), QKeySequence(Qt::CTRL | Qt::Key_H),
                 [this] {
                     if (!m_activePanel)
                         return;
                     m_activePanel->toggleHiddenFiles();
                     m_settings.setShowHiddenFiles(m_activePanel->model()->showHiddenFiles());
                 });
    bindShortcut("calcSize", tr("Calculate Folder Size"),
                 QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Return), [this] { calculateSizes(); });
    bindShortcut("swapPanels", tr("Swap Panels"), QKeySequence(Qt::CTRL | Qt::Key_U),
                 [this] { swapPanels(); });
    bindShortcut("syncOther", tr("Same Directory in Other Panel"),
                 QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { syncOtherPanelToActive(); });
    bindShortcut("quickView", tr("Quick View"), QKeySequence(Qt::CTRL | Qt::Key_Q),
                 [this] { toggleQuickView(); });
    bindShortcut("undo", tr("Undo Last Operation"), QKeySequence(Qt::CTRL | Qt::Key_Z),
                 [this] { undoLast(); });
    // Ctrl+F1 follows the TC convention where the Ctrl+F<n> block selects the
    // view mode. Panel-scoped like the menu entry, so the two panels can sit in
    // different modes.
    bindShortcut("toggleViewMode", tr("List / Thumbnail View"),
                 QKeySequence(Qt::CTRL | Qt::Key_F1), [this] {
                     if (m_activePanel)
                         m_activePanel->toggleViewMode();
                 });

    // --- Total Commander compatibility keys -------------------------------
    // Every sequence below was verified free in this application before being
    // taken, so these are pure additions: nothing that already worked changes.
    // Where the action already exists under a native shortcut (Properties on
    // F9, Find on Ctrl+F, ...) this registers a second, separately rebindable
    // command rather than moving the original, so both muscle memories work.
    // Reference: KEYBOARD.TXT shipped with Total Commander.
    bindShortcut("tcParentDir", tr("Parent Directory"),
                 QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this] {
                     if (m_activePanel)
                         m_activePanel->navigateUp();
                 });
    bindShortcut("tcOpenDir", tr("Open Directory or Archive"),
                 QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this] {
                     if (m_activePanel)
                         m_activePanel->activateCurrentEntry();
                 });
    bindShortcut("tcProperties", tr("Properties (Alt+Enter)"),
                 QKeySequence(Qt::ALT | Qt::Key_Return), [this] { showProperties(); });
    bindShortcut("tcFind", tr("Find Files (Alt+F7)"), QKeySequence(Qt::ALT | Qt::Key_F7),
                 [this] { openSearch(); });
    bindShortcut("tcCalcSpace", tr("Calculate Occupied Space"),
                 QKeySequence(Qt::CTRL | Qt::Key_L), [this] { calculateSizes(); });
    bindShortcut("tcRenameInPlace", tr("Rename (Shift+F6)"),
                 QKeySequence(Qt::SHIFT | Qt::Key_F6), [this] { renameCurrent(); });
    // The +/-/* selection keys are deliberately NOT registered here: FilePanel
    // already binds them (bare Key_Plus/Minus/Asterisk, so they fire from the
    // keypad and the main row alike). A second binding here would only make Qt
    // report an ambiguous overload and fire neither.
    //
    // Ctrl+F3..F6 sort by name / extension / date / size. The column numbers are
    // FileSystemModel's own order (Name, Ext, Size, Modified, ...), so date and
    // size are deliberately not in key order here.
    struct SortKey { const char *id; int key; int column; };
    const SortKey sortKeys[] = {
        {"tcSortName", Qt::Key_F3, 0},
        {"tcSortExt", Qt::Key_F4, 1},
        {"tcSortDate", Qt::Key_F5, 3},
        {"tcSortSize", Qt::Key_F6, 2},
    };
    const QString sortLabels[] = {tr("Sort by Name"), tr("Sort by Extension"),
                                  tr("Sort by Date"), tr("Sort by Size")};
    for (int i = 0; i < 4; ++i) {
        const int column = sortKeys[i].column;
        bindShortcut(QString::fromLatin1(sortKeys[i].id), sortLabels[i],
                     QKeySequence(Qt::CTRL | sortKeys[i].key), [this, column] {
                         if (m_activePanel)
                             m_activePanel->view()->sortByHeaderSection(column);
                     });
    }
    bindShortcut("tcRootDir", tr("Go to Root Directory"),
                 QKeySequence(Qt::CTRL | Qt::Key_Backslash), [this] { navigateToRoot(); });
    bindShortcut("tcTargetDir", tr("Go to Other Panel's Directory"),
                 QKeySequence(Qt::CTRL | Qt::Key_I), [this] { syncActiveToOtherPanel(); });
    bindShortcut("tcOpenInNewTab", tr("Open Directory in New Tab"),
                 QKeySequence(Qt::CTRL | Qt::Key_Up), [this] { openCurrentEntryInNewTab(); });
    bindShortcut("tcContextMenu", tr("Show Context Menu"),
                 QKeySequence(Qt::SHIFT | Qt::Key_F10), [this] { showContextMenuForCurrent(); });
    bindShortcut("tcCopyPathToCommandLine", tr("Copy Path to Command Line"),
                 QKeySequence(Qt::CTRL | Qt::Key_P), [this] { copyPathToCommandLine(); });
    bindShortcut("tcNewTextFile", tr("New Text File"), QKeySequence(Qt::SHIFT | Qt::Key_F4),
                 [this] { createNewTextFile(); });
    bindShortcut("tcCopySameDir", tr("Copy in Same Directory"),
                 QKeySequence(Qt::SHIFT | Qt::Key_F5), [this] { copyInSameDirectory(); });

    // F3-F8 are reassignable slots: the key and the bottom-bar button both run
    // whichever command the slot points at.
    const char *fkeyDefaults[6] = {"view", "edit", "copy", "move", "mkdir", "delete"};
    for (int i = 0; i < 6; ++i) {
        // Record each command's default F-key so the change dialog can show it.
        m_commands.setDefaultSequence(QString::fromLatin1(fkeyDefaults[i]),
                                      QKeySequence(static_cast<int>(Qt::Key_F3) + i));
        m_fkeyCommands[i] =
            m_settings.functionKeyCommand(i, QString::fromLatin1(fkeyDefaults[i]));
        if (!m_commands.isBuilt()) {
            auto *sc = new QShortcut(QKeySequence(static_cast<int>(Qt::Key_F3) + i), this);
            sc->setContext(Qt::WindowShortcut);
            connect(sc, &QShortcut::activated, this, [this, i] { runFunctionKey(i); });
        }
    }
    if (!m_commands.isBuilt()) {
        connect(m_functionKeyBar, &FunctionKeyBar::activated, this, &MainWindow::runFunctionKey);
        connect(m_functionKeyBar, &FunctionKeyBar::changeRequested, this,
                &MainWindow::changeFunctionKey);
        connect(m_functionKeyBar, &FunctionKeyBar::leadingActivated, this,
                [this] { runExtraKey(QStringLiteral("leading")); });
        connect(m_functionKeyBar, &FunctionKeyBar::trailingActivated, this,
                [this] { runExtraKey(QStringLiteral("trailing")); });
        connect(m_functionKeyBar, &FunctionKeyBar::leadingChangeRequested, this,
                [this] { changeExtraKey(QStringLiteral("leading")); });
        connect(m_functionKeyBar, &FunctionKeyBar::trailingChangeRequested, this,
                [this] { changeExtraKey(QStringLiteral("trailing")); });
        m_commands.markBuilt();
    }
    updateFunctionKeyLabels();
    updateExtraKeyButtons();
}

void MainWindow::showProperties() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
    // Anything but a local tab: build the dialog from the cached FileInfos so
    // owner/group/permissions/size come from the provider's listing. A local
    // QFileInfo over a server path (or an in-archive one) reports nothing --
    // which is how a three-file selection came out as "0 B" -- and the dialog's
    // permission grid must stay read-only, since chmod-ing such a path either
    // fails or hits a same-named local file.
    //
    // Gated on isLocalFilesystem() rather than displayName(): an archive tab has
    // no displayName either, and its entries are no more local than a share's.
    FileProvider *prov = m_activePanel->model()->provider();
    if (prov && !prov->isLocalFilesystem()) {
        const QVector<FileInfo> infos = m_activePanel->selectedEntryInfos();
        if (!infos.isEmpty()) {
            PropertiesDialog dlg(infos, this);
            dlg.exec();
            return;
        }
    }
    PropertiesDialog dlg(paths, this);
    if (dlg.exec() == QDialog::Accepted)
        m_activePanel->refresh();
}

void MainWindow::calculateSizes() {
    if (m_activePanel)
        m_activePanel->calculateDirSizes();
}

void MainWindow::chooseGlobalFont() {
    QFont initial = Typography::chromeFont(m_settings);
    bool accepted = false;
    const QFont selected = ttc::getFont(&accepted, initial, this, tr("Choose Font"));
    if (!accepted)
        return;
    m_settings.setGlobalFontFamily(selected.family());
    applyInterfaceTypography();
    for (FilePanel *panel : {m_leftPanel, m_rightPanel})
        panel->setListTypography(selected.family(), m_settings.listFontSize());
    if (m_quickView) {
        m_quickView->setContentFontFamily(selected.family());
        m_quickView->setContentFontSize(m_settings.listFontSize());
    }
    QTimer::singleShot(0, this, &MainWindow::buildTitleBarMenus);
}

void MainWindow::applyInterfaceTypography() {
    const QFont chrome = Typography::chromeFont(m_settings);
    Typography::applyApplicationFont(chrome);
    Typography::applyChromeFont(this, chrome);
    Typography::applyChromeFont(m_leftPanel, chrome);
    Typography::applyChromeFont(m_rightPanel, chrome);
    // The panels' own chrome (tab strip, address row, quick filter, status bar)
    // needs the font assigned widget by widget; the file views inside them keep
    // the list font, which is why the panels cannot go through
    // applyChromeFontToTree().
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        if (panel)
            panel->applyChromeFont(chrome);
    }
    // Recursive, and including the title bar (whose menu buttons -- Interface /
    // Tools / Config -- were simply never reached from here at all). See
    // applyChromeFontToTree for why plain inheritance does not carry the new font
    // down to the buttons inside these bars.
    Typography::applyChromeFontToTree(m_titleBar, chrome);
    Typography::applyChromeFontToTree(m_commandBar, chrome);
    Typography::applyChromeFontToTree(m_functionKeyBar, chrome);
    // Preview toolbars are chrome, so they follow this font -- but the previewed
    // content is not, so QuickView applies it only to its toolbars rather than
    // being handed to applyChromeFontToTree(). Covers the embedded pane and any
    // open F3 viewer window, the same pair refreshPhosphor() walks.
    if (m_quickView)
        m_quickView->applyChromeFont(chrome);
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *viewer = qobject_cast<ViewerWindow *>(w))
            viewer->applyChromeFont(chrome);
    }
    // The title-bar menus were each given an explicit font at construction
    // (buildTitleBarMenus() calls applyChromeFont(menu, m_settings) so a freshly
    // built menu isn't stuck on some earlier default) -- but an explicitly-set
    // font stops a widget from auto-tracking QApplication::setFont() afterward, so
    // without this the menus themselves stayed pinned at whatever font they had
    // when last (re)built. MenuChromeSynchronizer only re-syncs the *embedded*
    // font-size-stepper widgets (Typography.cpp) to the menu's *own* current
    // font, so if the menu's own font never changes, resyncing to it is a no-op:
    // this is why the menu-font-size and list-font-size caption rows kept
    // displaying a stale size after being adjusted with a menu open, even though
    // adjusting them did correctly change every other chrome surface's font.
    for (QMenu *menu : {m_interfaceMenu, m_toolsMenu, m_configMenu}) {
        Typography::applyChromeFont(menu, chrome);
        // Force the embedded font-size-stepper rows to match right now, rather than
        // trust MenuChromeSynchronizer's own deferred re-sync (installed on this
        // exact menu the first time one of those rows was built) to fire in time --
        // this call is exactly what that sync would eventually do, done eagerly.
        Typography::refreshOpenMenuChrome(menu);
    }
}

void MainWindow::calculateChecksums() {
    if (!m_activePanel)
        return;

    // A network/archive tab's paths are the server's (or the archive's), so
    // QFileInfo::isFile() rejected every one of them and the user was told they
    // had selected nothing -- and where a local file happened to share the name,
    // it was that file's bytes that got hashed under the remote file's label.
    // Hashing is worth having here, so stream the real bytes through the
    // provider rather than refusing.
    FileProvider *prov = m_activePanel->model()->provider();
    if (prov && !prov->isLocalFilesystem()) {
        QVector<FileInfo> entries;
        for (const FileInfo &info : m_activePanel->selectedEntryInfos())
            if (info.isValid() && !info.isDir())
                entries.append(info);
        if (entries.isEmpty()) {
            ttc::information(this, tr("Checksums"), tr("Select one or more files first."));
            return;
        }
        if (!prov->canStream()) {
            ttc::warning(this, tr("Checksums"),
                         tr("This connection cannot read file contents, so checksums "
                            "cannot be computed for these files."));
            return;
        }
        auto *dlg = new ChecksumDialog(entries, m_activePanel->model()->providerPtr(), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        return;
    }

    // Files only (checksums of a directory are meaningless).
    QStringList files;
    for (const QString &p : m_activePanel->selectedPaths())
        if (QFileInfo(p).isFile())
            files.append(p);
    if (files.isEmpty()) {
        ttc::information(this, tr("Checksums"),
                                 tr("Select one or more files first."));
        return;
    }
    // Non-modal: hashing runs on a background thread inside the dialog.
    auto *dlg = new ChecksumDialog(files, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::secureWipeSelected() {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || defined(Q_OS_WIN)
    if (!m_activePanel)
        return;

    // Secure wipe is the one operation that bypasses the provider entirely: the
    // dialog walks the selection with QDir and overwrites the bytes with QFile.
    // That is only meaningful -- and only safe -- when the paths really are
    // local-filesystem paths. Everywhere else the very same code finds and
    // shreds a same-named LOCAL file while the prompt names a remote one (an
    // archive holding "etc/passwd" browses as the path "/etc/passwd"), or
    // matches nothing and still reports "Wiped" for a file it never touched.
    //
    // So refuse, and say why. This is not a gap to fill in later: on a server
    // the data blocks belong to the server, and no client can promise anything
    // about them. The check must come before blockArchiveWrite() -- that one's
    // "copy files out to a folder to modify them" is sound advice for editing
    // and actively wrong here, since wiping the copy leaves the original.
    FileProvider *prov = m_activePanel->model()->provider();
    if (!prov || !prov->isLocalFilesystem()) {
        // Kept short and hard-wrapped: ttc::message sizes the box to the text,
        // so a long paragraph stretches it across the whole screen.
        const QString conn = m_activePanel->model()->connectionId();
        QString why;
        if (!conn.isEmpty())
            why = tr("These items are on %1.\n"
                     "The server owns their disk blocks, so overwriting them\n"
                     "from here cannot guarantee the originals are gone.\n"
                     "Delete them remotely instead.")
                      .arg(conn);
        else if (m_activePanel->isArchive())
            why = tr("These items are entries inside an archive,\n"
                     "not files on this disk.\n"
                     "To destroy them, wipe the archive file itself\n"
                     "from the folder that holds it.");
        else
            why = tr("This tab is not the local filesystem,\n"
                     "so there are no on-disk bytes here to overwrite.");
        ttc::warning(this, tr("Secure Wipe"),
                     tr("Secure wipe is only available on local files.\n\n%1").arg(why));
        return;
    }
    if (blockArchiveWrite(m_activePanel)) // backstop: panel says archive, provider didn't
        return;

    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;

    const qint64 total = sumSizes(m_activePanel->model(), paths);
    if (!SecureWipeConfirmationDialog::ask(this, paths, total))
        return;

    // Non-modal: overwriting runs on a background thread inside the dialog.
    auto *dlg = new SecureWipeDialog(paths, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &SecureWipeDialog::filesChanged, this, [this] {
        m_leftPanel->refresh();
        m_rightPanel->refresh();
    });
    dlg->show();
#else
    ttc::information(this, tr("Secure Wipe"),
                     tr("Secure wipe is not available on this platform."));
#endif
}

void MainWindow::syncOtherPanelToActive() {
    if (!m_activePanel)
        return;
    FilePanel *other = otherPanel(m_activePanel);
    if (!other)
        return;

    // A path only means something to the backend it came from. Both panels on
    // the same connection (or both local) can share one; otherwise the other
    // panel would resolve it against its own backend -- and since every backend
    // here uses POSIX-rooted paths, a path like "/home" exists on a share AND on
    // this machine, so the panel would quietly land somewhere else rather than
    // fail. Say so instead of moving to the wrong place.
    if (m_activePanel->connectionId() != other->connectionId()) {
        ttc::warning(this, tr("Same Directory in Other Panel"),
                     tr("The two panels are on different connections, so this directory has no "
                        "meaning in the other one. Use Swap Panels (Ctrl+U) to move this "
                        "connection across instead."));
        return;
    }
    other->navigateTo(m_activePanel->currentPath());
}

void MainWindow::syncActiveToOtherPanel() {
    if (!m_activePanel)
        return;
    FilePanel *other = otherPanel(m_activePanel);
    if (!other)
        return;
    // Same cross-backend hazard as syncOtherPanelToActive(), just in the other
    // direction: every backend here uses POSIX-rooted paths, so a path from the
    // other panel would silently resolve against this panel's own backend and
    // land somewhere that merely happens to share the name.
    if (m_activePanel->connectionId() != other->connectionId()) {
        ttc::warning(this, tr("Go to Other Panel's Directory"),
                     tr("The two panels are on different connections, so that directory has no "
                        "meaning in this one. Use Swap Panels (Ctrl+U) to move the connection "
                        "across instead."));
        return;
    }
    m_activePanel->navigateTo(other->currentPath());
}

void MainWindow::navigateToRoot() {
    if (!m_activePanel)
        return;
    // Pure string work, deliberately: on a network or archive tab this path
    // belongs to that backend, and asking QDir/QFileInfo about it would probe
    // THIS machine's filesystem instead (see the path-is-not-a-local-path note
    // in CLAUDE.md). Both shapes that can occur are handled directly.
    const QString path = QDir::fromNativeSeparators(m_activePanel->currentPath());
    QString root;
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
        root = path.left(2) + QLatin1Char('/'); // drive-letter path: X:/
    else if (path.startsWith(QLatin1Char('/')))
        root = QStringLiteral("/");
    if (!root.isEmpty() && QDir::cleanPath(root) != QDir::cleanPath(path))
        m_activePanel->navigateTo(root);
}

void MainWindow::openCurrentEntryInNewTab() {
    if (!m_activePanel || !m_activePanel->currentEntryIsDir())
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty())
        return;
    m_activePanel->newTab(); // opens AND activates, at the current directory
    m_activePanel->navigateTo(path);
}

void MainWindow::showContextMenuForCurrent() {
    if (!m_activePanel)
        return;
    QAbstractItemView *view = m_activePanel->activeView();
    if (!view)
        return;
    // Anchor the menu on the focused row rather than the mouse: this is the
    // keyboard route, and the pointer may be anywhere (or on the other panel).
    const QModelIndex current = view->currentIndex();
    if (current.isValid()) {
        const QRect rect = view->visualRect(current);
        showFileContextMenu(m_activePanel, rect.isValid() ? rect.center() : QPoint(0, 0));
    } else {
        showBlankContextMenu(m_activePanel, QPoint(0, 0));
    }
}

void MainWindow::copyPathToCommandLine() {
    if (!m_activePanel || !m_commandBar)
        return;
    // TC appends the CURRENT DIRECTORY (Ctrl+P), not the file under the cursor
    // -- that is Ctrl+Enter, a separate command-line key.
    m_commandBar->appendText(m_activePanel->currentPath());
}

void MainWindow::createNewTextFile() {
    if (!m_activePanel || blockArchiveWrite(m_activePanel))
        return;
    if (blockWithoutWorkingDirectory(m_activePanel, tr("New Text File")))
        return;
    // Creating the file needs a real writable path, which only a local tab has.
    if (m_activePanel->model()->providerPtr().get() != LocalFileProvider::instance()) {
        ttc::information(this, tr("New Text File"),
                         tr("New files can only be created on a local tab."));
        return;
    }

    bool ok = false;
    const QString name = ttc::getText(this, tr("New Text File"), tr("File name:"),
                                      QLineEdit::Normal, QStringLiteral("new.txt"), &ok);
    if (!ok || name.isEmpty())
        return;

    const QString target = QDir(m_activePanel->currentPath()).filePath(name);
    if (QFileInfo::exists(target)) {
        ttc::warning(this, tr("New Text File"), tr("%1 already exists.").arg(name));
        return;
    }
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly)) { // creates it empty
        ttc::warning(this, tr("New Text File"),
                     tr("Could not create %1: %2").arg(name, file.errorString()));
        return;
    }
    file.close();

    m_activePanel->refresh();
    m_activePanel->selectPathAfterReload(target);
    auto *editor = new TextEditor();
    if (!editor->loadFile(target)) {
        delete editor; // loadFile already reported why
        return;
    }
    editor->resize(900, 700);
    editor->show();
}

void MainWindow::copyInSameDirectory() {
    if (!m_activePanel || blockArchiveWrite(m_activePanel))
        return;
    const QStringList sources = m_activePanel->selectedPaths();
    if (sources.size() != 1) {
        ttc::information(this, tr("Copy in Same Directory"),
                         tr("Select exactly one item to copy under a new name."));
        return;
    }
    if (m_activePanel->model()->providerPtr().get() != LocalFileProvider::instance()) {
        ttc::information(this, tr("Copy in Same Directory"),
                         tr("This is only available on a local tab."));
        return;
    }

    const QString dir = m_activePanel->currentPath();
    bool ok = false;
    const QString newName =
        ttc::getText(this, tr("Copy in Same Directory"), tr("Copy to:"), QLineEdit::Normal,
                     QFileInfo(sources.first()).fileName(), &ok);
    if (!ok || newName.isEmpty())
        return;
    const QString target = QDir(dir).filePath(newName);
    if (QDir::cleanPath(target) == QDir::cleanPath(sources.first()))
        return; // same name: nothing to do

    m_pendingDestPanel = m_activePanel;
    m_pendingDestPaths = QStringList{target};
    ensureTransferProgressDialog();
    m_queue->enqueueCopyAs(sources.first(), target);
}

void MainWindow::swapPanels() {
    if (m_quickViewActive && m_quickView) {
        const QList<int> sizes = m_panelSplitter->sizes();
        FilePanel *visible = otherPanel(m_quickViewPanel);
        const int visibleIndex = m_panelSplitter->indexOf(visible);

        // The preview must trade places with the visible FilePanel, not with the
        // parked panel it replaced. Detach both visible widgets before inserting
        // them so QSplitter never makes the parked panel visible as an
        // intermediate replacement.
        m_quickView->setParent(this);
        visible->setParent(this);
        m_panelSplitter->insertWidget(visibleIndex, m_quickView);
        m_panelSplitter->insertWidget(m_quickViewIndex, visible);
        m_quickView->show();
        visible->show();

        // The original inactive panel remains parked. Only the preview's slot
        // changes, so closing it restores that parked panel on the swapped side.
        m_quickViewIndex = visibleIndex;

        // The visible panel stays active: its selection remains the preview
        // source, and focus must not jump to the panel hidden behind the preview.
        setActivePanel(visible);

        QList<int> swapped = sizes;
        std::reverse(swapped.begin(), swapped.end());
        m_panelSplitter->setSizes(swapped);
        updateQuickView();
        // showFile() may focus a freshly selected preview page. Restore keyboard
        // ownership to the file panel only after the preview has finished updating.
        visible->activeView()->setFocus();
        return;
    }

    // Exchange the backends, not the path strings: a remote path resolved by the
    // other panel's backend would land it somewhere else (every backend here is
    // POSIX-rooted, so "/home" resolves on a share and on this machine alike).
    m_leftPanel->exchangeLocationWith(m_rightPanel);
}

void MainWindow::openTerminalHere() {
    if (!m_activePanel)
        return;
    // A terminal's working directory is a path on THIS machine. A network or
    // archive tab's path belongs to the server or to the archive, and handing
    // it over either fails or -- worse -- silently opens a shell in a
    // same-named local directory.
    FileProvider *provider = m_activePanel->model()->provider();
    if (provider && !provider->isLocalFilesystem()) {
        ttc::information(this, tr("Open Terminal"),
                         tr("This tab is not showing local files, so there is no directory "
                            "on this computer for a terminal to start in."));
        return;
    }
    if (fc::openTerminalAt(m_activePanel->currentPath()))
        return;
    ttc::warning(this, tr("Open Terminal"), tr("No terminal emulator found."));
}

void MainWindow::openWithDefault() {
    if (!m_activePanel)
        return;
    // Delegate to the panel's activate logic: a directory (local, network or
    // archive) is entered in-place via its provider; a file is opened with the
    // associated app. The old QDesktopServices-on-local-path shortcut did
    // nothing for network tabs, so right-click "Open" couldn't change directory.
    m_activePanel->activateCurrentEntry();
}

void MainWindow::openWith() {
    // The keyboard command and any toolbar binding: no menu was opened, so go
    // straight to the one choice that always exists.
    chooseApplicationAndOpen();
}

void MainWindow::chooseApplicationAndOpen() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty())
        return;
#ifdef Q_OS_WIN
    const QString filter = tr("Programs (*.exe *.bat *.cmd *.com);;All files (*)");
#else
    const QString filter = tr("All files (*)");
#endif
    const QString app = QFileDialog::getOpenFileName(
        this, tr("Choose an application to open %1").arg(QFileInfo(path).fileName()),
        QString(), filter);
    if (app.isEmpty())
        return;
    fc::OpenWithHandler handler;
    handler.displayName = QFileInfo(app).completeBaseName();
    handler.program = app;
    runOpenWithHandler(handler, path);
}

void MainWindow::runOpenWithHandler(const fc::OpenWithHandler &handler, const QString &path) {
    if (!m_activePanel || path.isEmpty())
        return;
    FileProvider *prov = m_activePanel->model()->provider();
    if (prov && !prov->displayName().isEmpty()) {
        // Network tab: the application needs a real file, not the provider's
        // path, which names something on the server.
        fetchRemoteCopy(m_activePanel, path, [handler](const QString &localPath) {
            fc::launchOpenWithHandler(handler, localPath);
        });
        return;
    }
    if (!fc::launchOpenWithHandler(handler, path)) {
        ttc::warning(this, tr("Open With"),
                     tr("%1 could not be started.").arg(handler.displayName));
    }
}

QMenu *MainWindow::buildOpenWithMenu(const QString &path) {
    auto *menu = new QMenu(tr("Open With"));
    Typography::applyChromeFont(menu, m_settings);
    fillOpenWithMenu(menu, path);
    return menu;
}

void MainWindow::fillOpenWithMenu(QMenu *menu, const QString &path) {
    // Enumerating costs a shell round trip per registration, so it happens when
    // the submenu is opened rather than on every right-click.
    menu->clear();
    QVector<fc::OpenWithHandler> handlers;
    // An archive or network entry has no local name to look a type up by, but
    // its EXTENSION is still meaningful -- the applications registered for a
    // .png are the same ones whether the file sits on disk or inside a zip.
    if (!path.isEmpty())
        handlers = fc::openWithHandlers(path);

    // Through IconCache, not QFileIconProvider: these are the system's own
    // artwork like a file-type icon, and they have to answer to the same tint
    // as every other icon in the window -- straight from the provider they came
    // out in full colour beside a menu of phosphor-green glyphs.
    //
    // It is cache-only, so the first time a type's applications are listed the
    // icons are missing; the warm-up below fills them in and the actions pick
    // them up while the menu is still open.
    QVector<QPointer<QAction>> pending;
    QStringList programs;
    const auto addHandler = [&](QMenu *target, const fc::OpenWithHandler &handler) {
        QAction *action = target->addAction(handler.displayName);
        if (!handler.program.isEmpty()) {
            const QIcon icon = IconCache::instance().systemIconForPath(handler.program);
            if (icon.isNull()) {
                pending.append(action);
                programs.append(handler.program);
            } else {
                action->setIcon(icon);
            }
        }
        connect(action, &QAction::triggered, this,
                [this, handler, path]() { runOpenWithHandler(handler, path); });
    };

    int recommended = 0;
    for (const fc::OpenWithHandler &handler : handlers) {
        if (!handler.recommended)
            break; // tidyOpenWithHandlers() puts them all first
        addHandler(menu, handler);
        ++recommended;
    }

    if (recommended < handlers.size()) {
        // Everything else the system can launch. It goes behind one more level
        // only when there is a recommended list to keep at the top; with
        // nothing registered for the type, burying the whole list would leave
        // an "Open With" menu that appears to offer nothing.
        QMenu *rest = menu;
        if (recommended > 0) {
            menu->addSeparator();
            rest = menu->addMenu(tr("Other Applications"));
            Typography::applyChromeFont(rest, m_settings);
        }
        for (int i = recommended; i < handlers.size(); ++i)
            addHandler(rest, handlers[i]);
    }

    if (!menu->isEmpty())
        menu->addSeparator();
    QAction *browse = menu->addAction(tr("Choose Another Application…"));
    connect(browse, &QAction::triggered, this, [this]() { chooseApplicationAndOpen(); });

    if (!programs.isEmpty())
        warmOpenWithIcons(programs, pending);
}

void MainWindow::warmOpenWithIcons(const QStringList &programs,
                                   const QVector<QPointer<QAction>> &actions) {
    // Same shape as warmDriveIcons(): the shell query goes to a worker, because
    // asking it here would stall the menu on whatever medium the application
    // happens to live on. The actions are held weakly -- the menu may well be
    // gone by the time this lands, which is not a failure.
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this,
            [watcher, programs, actions]() {
                watcher->deleteLater();
                for (int i = 0; i < actions.size() && i < programs.size(); ++i) {
                    if (!actions[i])
                        continue;
                    const QIcon icon = IconCache::instance().systemIconForPath(programs[i]);
                    if (!icon.isNull())
                        actions[i]->setIcon(icon);
                }
            });
    watcher->setFuture(QtConcurrent::run([programs]() {
        for (const QString &program : programs)
            IconCache::instance().warmSystemIconForPath(program);
    }));
}

void MainWindow::compareDirectories() {
    auto gather = [](FileSystemModel *model) {
        QHash<QString, QDateTime> map;
        for (int r = 0; r < model->rowCount(); ++r) {
            if (model->isParentEntry(r))
                continue;
            const FileInfo fi = model->fileInfoAt(r);
            if (fi.isValid() && !fi.isDir()) // compare files by name+mtime
                map.insert(fi.name(), fi.modified());
        }
        return map;
    };
    FileSystemModel *left = m_leftPanel->model();
    FileSystemModel *right = m_rightPanel->model();
    const QHash<QString, QDateTime> leftMap = gather(left);
    const QHash<QString, QDateTime> rightMap = gather(right);
    left->setCompareStatus(FileSystemModel::compareStatuses(leftMap, rightMap));
    right->setCompareStatus(FileSystemModel::compareStatuses(rightMap, leftMap));
}

void MainWindow::recordMoveUndo(const QStringList &sources, const QString &destDir) {
    if (sources.isEmpty())
        return;
    m_lastUndo = UndoRecord{};
    m_lastUndo.type = UndoRecord::Move;
    m_lastUndo.restoreDir = QFileInfo(sources.first()).absolutePath();
    for (const QString &s : sources)
        m_lastUndo.movedPaths << QDir(destDir).filePath(QFileInfo(s).fileName());
}

void MainWindow::undoLast() {
    const UndoRecord rec = m_lastUndo;
    m_lastUndo = UndoRecord{}; // consume, so undo isn't itself undoable
    switch (rec.type) {
    case UndoRecord::Rename:
        ensureTransferProgressDialog();
        if (rec.provider)
            m_queue->enqueueProviderRename(rec.provider, rec.fromPath, rec.toName);
        else
            m_queue->enqueueRename(rec.fromPath, rec.toName);
        break;
    case UndoRecord::Move:
        ensureTransferProgressDialog();
        m_queue->enqueueMove(rec.movedPaths, rec.restoreDir);
        break;
    case UndoRecord::None:
        break;
    }
}

void MainWindow::toggleQuickView() {
    // Preserve the left/right split ratio across the widget swap: replaceWidget
    // otherwise redistributes space based on the new widget's size hint.
    const QList<int> sizes = m_panelSplitter->sizes();

    if (m_quickViewActive && m_quickView) {
        // Stop any playing media so it doesn't keep running behind the hidden pane.
        m_quickView->stopPlayback();
        // Put the previewed panel back where it was.
        m_panelSplitter->replaceWidget(m_quickViewIndex, m_quickViewPanel);
        m_quickViewPanel->show();
        m_quickView->setParent(this);
        m_quickView->hide();
        m_quickViewActive = false;
        m_quickViewPanel = nullptr;
        m_panelSplitter->setSizes(sizes);
        if (m_activePanel)
            m_activePanel->activeView()->setFocus();
        return;
    }
    if (!m_activePanel)
        return;
    QuickView *const quickView = ensureQuickView();
    // Replace the *inactive* panel with the preview.
    m_quickViewPanel = otherPanel(m_activePanel);
    m_quickViewIndex = m_panelSplitter->indexOf(m_quickViewPanel);
    m_panelSplitter->replaceWidget(m_quickViewIndex, quickView);
    quickView->show();
    m_quickViewActive = true;
    m_panelSplitter->setSizes(sizes);
    updateQuickView();
}

namespace {
// Downloads a remote file to `outPath`, a real local file, so the local viewers
// (or an external application) can open it -- images/text/media all need a real
// path. Streams through the provider on the calling worker thread, reporting
// bytes via `progressCb` and aborting promptly when *cancel becomes true.
// Returns outPath, or an empty string on failure/cancellation (the partial temp
// file is removed). The caller picks the name, so it can keep the file's own
// basename where that is user-visible.
QString downloadRemoteToTemp(FileProvider *provider, const QString &remotePath,
                             const QString &outPath, std::atomic<bool> *cancel,
                             qint64 total, const std::function<void(qint64, qint64)> &progressCb) {
    if (!provider || !provider->canStream())
        return {};
    FileHandle *h = provider->openRead(remotePath);
    if (!h)
        return {};
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        provider->closeHandle(h);
        return {};
    }
    QByteArray buf;
    buf.resize(64 * 1024);
    qint64 done = 0, lastReported = 0, n = 0;
    bool ok = true;
    while (true) {
        if (cancel && cancel->load()) { // user pressed Stop
            ok = false;
            break;
        }
        n = provider->read(h, buf.data(), buf.size());
        if (n <= 0)
            break; // 0 = EOF, <0 = error (handled below)
        if (out.write(buf.constData(), n) != n) {
            ok = false;
            break;
        }
        done += n;
        // Throttle progress to ~1 MiB steps so the event queue isn't flooded.
        if (progressCb && done - lastReported >= 1024 * 1024) {
            progressCb(done, total);
            lastReported = done;
        }
    }
    if (n < 0)
        ok = false;
    provider->closeHandle(h);
    out.close();
    if (!ok) {
        QFile::remove(outPath);
        return {};
    }
    if (progressCb)
        progressCb(done, total); // final 100%
    return outPath;
}
} // namespace

QString MainWindow::ensurePreviewTempDir() {
    return m_scratch.preview();
}

void MainWindow::updateQuickView() {
    if (!m_quickViewActive || !m_activePanel)
        return;
    // An archive under the cursor previews from its raw path (a header scan).
    const QString entry = m_activePanel->currentEntryPath();
    if (ArchiveHandler::isSupportedArchive(entry)) {
        m_quickView->showFile(entry);
        return;
    }

    FileProvider *prov = m_activePanel->model()->provider();
    const bool network = prov && !prov->displayName().isEmpty();
    if (!network) {
        // A local file is its own answer. An archive entry may not be: the
        // first one touched in a non-zip archive extracts the WHOLE archive,
        // which for a few hundred images is seconds -- and it used to be
        // seconds with the window frozen and nothing on screen to explain it.
        const QString ready = m_activePanel->currentPreviewPathIfReady();
        if (!ready.isEmpty() || !m_activePanel->isArchive()) {
            m_quickView->showFile(ready);
            return;
        }
        m_quickView->showPreparing(QFileInfo(entry).fileName());
        m_activePanel->beginPreviewExtraction();
        return;
    }

    // Network file: the remote path isn't a real local file, so download it to a
    // temp file on a worker thread (never blocking the GUI) and show it when
    // ready. Skip directories and oversized files (using the cached listing, no
    // remote round-trip). A newer cursor position supersedes an in-flight one.
    ++m_previewReqId;
    const quint64 reqId = m_previewReqId;
    m_previewRunning = false;
    m_quickView->showFile(QString()); // blank while loading / when unpreviewable
    if (entry.isEmpty() || m_activePanel->currentEntryIsDir())
        return;

    // Video streams straight off the backend: mpv reads the few megabytes it
    // decodes through the provider instead of waiting out a whole download.
    // Nothing below this point runs for it -- including the size ceiling, which
    // only ever existed because previewing meant fetching the file entire.
    // Windows Media Foundation deliberately never publishes provider streams:
    // WebDAV videos fall through to the existing temp-file download below so
    // the engine opens a seekable file:// URL, while SFTP/FTP remain non-streaming.
#if FILECOMMANDER_MEDIA_BACKEND_MPV
    if (QuickView::canStreamPreview(entry) && entry != m_streamFailedEntry) {
        const QString url =
            MpvStreamSource::publish(m_activePanel->model()->providerPtr(), entry);
        if (!url.isEmpty()) {
            m_quickView->showFile(url);
            return;
        }
    }
#endif

    // Everything else still has to become a real local file first: Poppler, the
    // office converter, QImageReader and the archive reader all open a path.
    static constexpr qint64 kMaxPreviewBytes = 100LL * 1024 * 1024; // don't fetch huge files
    const qint64 total = m_activePanel->currentEntrySize();
    if (total > kMaxPreviewBytes)
        return;
    const QString destDir = ensurePreviewTempDir();
    if (destDir.isEmpty())
        return;

    m_previewRunning = true;
    m_previewName = QFileInfo(entry).fileName();
    // Fresh cancel flag for this download; the Stop button flips it.
    m_previewCancel = std::make_shared<std::atomic<bool>>(false);

    // If the download hasn't finished within 0.5s, show the download page with a
    // progress bar and a Stop button (per the requested UX) so the pane isn't
    // just blank. A quick download finishes first and never shows it.
    const QString name = m_previewName;
    QTimer::singleShot(500, this, [this, reqId, name] {
        if (reqId == m_previewReqId && m_previewRunning)
            m_quickView->showDownloading(name);
    });

    std::shared_ptr<FileProvider> provider = m_activePanel->model()->providerPtr();
    std::shared_ptr<std::atomic<bool>> cancel = m_previewCancel;
    MainWindow *self = this; // lives for the app's lifetime; queued calls hop to GUI thread
    QtConcurrent::run([self, provider, entry, destDir, reqId, cancel, total] {
        auto progressCb = [self, reqId](qint64 done, qint64 tot) {
            QMetaObject::invokeMethod(self, "onPreviewProgress", Qt::QueuedConnection,
                                      Q_ARG(quint64, reqId), Q_ARG(qint64, done),
                                      Q_ARG(qint64, tot));
        };
        // The request id in the name keeps concurrent previews from colliding.
        const QString outPath = QDir(destDir).filePath(
            QStringLiteral("%1_%2").arg(reqId).arg(QFileInfo(entry).fileName()));
        const QString tmp =
            downloadRemoteToTemp(provider.get(), entry, outPath, cancel.get(), total, progressCb);
        const bool cancelled = cancel->load();
        QMetaObject::invokeMethod(self, "onPreviewDone", Qt::QueuedConnection,
                                  Q_ARG(quint64, reqId), Q_ARG(QString, tmp),
                                  Q_ARG(bool, cancelled));
    });
}

void MainWindow::onPreviewProgress(quint64 reqId, qint64 done, qint64 total) {
    if (reqId != m_previewReqId || !m_previewRunning)
        return; // superseded / already done
    m_quickView->setDownloadProgress(done, total);
}

void MainWindow::onPreviewDone(quint64 reqId, const QString &tempPath, bool cancelled) {
    if (reqId != m_previewReqId)
        return; // a newer selection took over; this result is stale
    m_previewRunning = false;
    if (cancelled)
        m_quickView->showDownloadCancelled(m_previewName);
    else
        m_quickView->showFile(tempPath); // empty path -> "select a file" placeholder
}

void MainWindow::cancelPreviewDownload() {
    if (m_previewCancel)
        m_previewCancel->store(true); // the worker's read loop aborts on the next chunk
    m_previewRunning = false;
    m_quickView->showDownloadCancelled(m_previewName);
}

QString MainWindow::ensureOpenTempDir() {
    return m_scratch.openWith();
}

QString MainWindow::ensureArchiveTempDir() {
    return m_scratch.archive();
}

void MainWindow::browseRemoteArchive(FilePanel *panel, const QString &path) {
    if (!panel || path.isEmpty())
        return;
    // libarchive (and unsquashfs, and 7z) open an archive by file name, so the
    // whole question is where that name comes from.
    //
    // A gvfs mount answers it without moving a byte, and the difference is not
    // marginal: a 686 MB 7z on SMB took 10.64 s to download and 44 ms to open
    // through the mount point (240x); a 24.8 MB WebDAV zip, 430 ms against
    // 325 ms. The mount is genuinely seekable, which is what these formats need
    // -- they read a central directory at the end of the file and then jump.
    // Downloading stays as the fallback for when there is no mount to be had.
    QPointer<FilePanel> guard(panel);
    // Resolving (and any download after it) takes a while, and the user is free
    // to move the panel elsewhere meanwhile. Entering then would read ".." off
    // whatever backend the panel had drifted onto, so the connection is checked
    // on the way back in and a panel that has left is simply not disturbed.
    const QString conn = panel->connectionId();
    resolveRealPath(panel, path, [this, guard, path, conn](const QString &real) {
        if (!guard || guard->connectionId() != conn || guard->isArchive())
            return;
        // ownsLocalCopy = false, and it matters more here than anywhere else:
        // this is the user's own archive seen through the mount, so the "delete
        // the copy on the way out" that a download gets would delete the file
        // off their server.
        if (!real.isEmpty() && guard->enterArchive(real, path, false))
            return;
        // No mount, or libarchive could not open it through one. Fall through to
        // the download, which ends exactly where this used to: a browse if the
        // bytes turn out to be a readable archive, the desktop's handler if not.
        // Retrying costs a download that would have happened anyway.
        browseRemoteArchiveByDownload(guard, path);
    });
}

void MainWindow::browseRemoteArchiveByDownload(FilePanel *panel, const QString &path) {
    if (!panel || path.isEmpty())
        return;
    const QString name = QFileInfo(path).fileName();
    QPointer<FilePanel> guard(panel);
    const QString conn = panel->connectionId();
    fetchRemoteCopy(panel, path, [this, guard, path, name, conn](const QString &localPath) {
        if (!guard)
            return; // the panel went away while the copy was in flight
        if (guard->connectionId() != conn || guard->isArchive()) {
            QFile::remove(localPath);
            QDir().rmdir(QFileInfo(localPath).absolutePath());
            return;
        }
        // The copy belongs to the browse: enterArchive() takes its lifetime and
        // deletes it on the way out. If the file turns out not to be a readable
        // archive, fall back to the desktop's handler, which is what a local tab
        // does for the same file.
        if (guard->enterArchive(localPath, path, true))
            return;
        // Named like an archive but not readable as one (corrupt, or a format
        // libarchive won't open). The copy is already here, so hand it straight
        // to the desktop rather than fetching the same bytes a second time.
        openLocalCopyWithDesktop(localPath, name);
    }, ensureArchiveTempDir());
}

void MainWindow::resolveRealPath(FilePanel *panel, const QString &path,
                                 std::function<void(const QString &)> then) {
    if (!then)
        return;
    FileProvider *prov = panel ? panel->model()->provider() : nullptr;
    if (!prov || path.isEmpty()) {
        then(QString());
        return;
    }
    // A local backend's paths already ARE paths on this machine. Ask the backend
    // rather than infer it: an archive tab has no displayName either, and its
    // in-archive "/etc/passwd" would otherwise be handed out as the real one.
    if (prov->isLocalFilesystem()) {
        then(path);
        return;
    }
    if (panel->isArchive()) {
        then(QString()); // an in-archive path names an entry, not a file
        return;
    }

    // Hold the provider by shared ownership for the duration: the panel may be
    // closed, or navigate to another connection, while the resolve is in flight.
    // Reading the location on the worker thread also keeps its mutex -- which a
    // large listing can hold for seconds -- off the GUI thread.
    std::shared_ptr<FileProvider> provider = panel->model()->providerPtr();
    QPointer<FilePanel> guard(panel);
    auto *watcher = new QFutureWatcher<QString>(this);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [watcher, guard, then = std::move(then)]() {
                QApplication::restoreOverrideCursor();
                const QString real = watcher->result();
                watcher->deleteLater();
                if (guard)
                    then(real);
            });
    watcher->setFuture(QtConcurrent::run([provider, path] {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
        return GvfsMounter::localPathFor(provider.get(), path);
#else
        Q_UNUSED(provider)
        Q_UNUSED(path)
        return QString();
#endif
    }));
}

void MainWindow::withLocalFile(FilePanel *panel, const QString &path,
                               std::function<void(const QString &)> then) {
    if (!then)
        return;
    FileProvider *prov = panel ? panel->model()->provider() : nullptr;
    if (!prov || path.isEmpty())
        return;
    if (prov->isLocalFilesystem()) {
        then(path); // already a real path; unchanged, synchronous behaviour
        return;
    }
    QPointer<FilePanel> guard(panel);
    resolveRealPath(panel, path, [this, guard, path, then](const QString &real) {
        if (!real.isEmpty()) {
            then(real);
            return;
        }
        // No mount (or an archive entry, which never has one). A copy costs the
        // bytes but always works, and fetchRemoteCopy reports its own failures.
        if (!guard)
            return;
        fetchRemoteCopy(guard, path, then);
    });
}

void MainWindow::openWithAssociatedApp(FilePanel *panel, const QString &path) {
    if (path.isEmpty())
        return;
    FileProvider *prov = panel ? panel->model()->provider() : nullptr;
    // Same test the preview pane uses: only network backends name a connection.
    const bool network = prov && !prov->displayName().isEmpty();
    if (!network) {
        // An AppImage straight from a browser download has no execute bit, and
        // opening it then does nothing at all -- or, worse, hands it to a text
        // editor. Offer to fix that here, where the user is already trying to
        // run it, rather than leaving them to work out why nothing happened.
        // Declining falls through to the normal open, which is what a user who
        // said "no" asked for.
        if (fc::ShellShortcuts::needsExecutableBit(path) && !offerExecutableBit(path))
            return;
        // Local tab: `path` already is a filesystem path -- unchanged behaviour.
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            ttc::warning(this, tr("Open"),
                                 tr("No application is associated with %1").arg(path));
        return;
    }

    // Network tab. Prefer the gvfs mount: it gives the file a real name on this
    // machine that the application opens directly off the server -- nothing is
    // copied, a multi-gigabyte file opens as fast as a small one, and edits are
    // saved back. The download below is the fallback, and it is a poor one by
    // comparison: the copy is a read-only snapshot, so anything the user edits
    // in it is lost.
    const QString name = QFileInfo(path).fileName();
    QPointer<FilePanel> guard(panel);
    resolveRealPath(panel, path, [this, guard, path, name](const QString &real) {
        if (!real.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(real)))
            return;
        // No mount, or the desktop had no handler for it. Either way the copy is
        // worth trying: it lands with the same name and extension, and a handler
        // that refused a path under /run/user/.../gvfs may still take this one.
        if (!guard)
            return;
        fetchRemoteCopy(guard, path, [this, name](const QString &localPath) {
            openLocalCopyWithDesktop(localPath, name);
        });
    });
}

void MainWindow::openLocalCopyWithDesktop(const QString &localPath, const QString &name) {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(localPath))) {
        ttc::warning(this, tr("Open"), tr("No application is associated with %1").arg(name));
        return;
    }
    // The copy is a snapshot: nothing writes it back to the server. Say so once
    // per session rather than on every open -- the read-only permission set in
    // onRemoteFetchDone is what actually stops an editor from discarding the
    // user's work, this explains why it happened.
    if (!m_remoteCopyNoticeShown) {
        m_remoteCopyNoticeShown = true;
        ttc::information(this, tr("Open"),
                         tr("%1 was downloaded to a read-only local copy, which is what the "
                            "application opened.\n\nChanges made to it are not saved back to "
                            "the server.")
                             .arg(name));
    }
}

void MainWindow::fetchRemoteCopy(FilePanel *panel, const QString &path,
                                 std::function<void(const QString &)> onReady,
                                 const QString &destRoot) {
    FileProvider *prov = panel ? panel->model()->provider() : nullptr;
    const QString name = QFileInfo(path).fileName();
    if (!prov || !prov->canStream()) {
        ttc::warning(this, tr("Open"),
                             tr("This connection cannot download files, so %1 cannot be opened "
                                "with a local application.")
                                 .arg(name));
        return;
    }
    const QString root = destRoot.isEmpty() ? ensureOpenTempDir() : destRoot;
    if (root.isEmpty()) {
        ttc::warning(this, tr("Open"),
                             tr("Could not create a temporary folder to download %1.").arg(name));
        return;
    }

    const quint64 reqId = ++m_openReqId;
    // One sub-directory per request, so the copy keeps the file's own name (the
    // application shows it in its title bar and picks its handler from the
    // extension) and a second open of the same file cannot overwrite a copy some
    // other application still has open.
    const QString destDir = QDir(root).filePath(QString::number(reqId));
    if (!QDir().mkpath(destDir)) {
        ttc::warning(this, tr("Open"),
                             tr("Could not create a temporary folder to download %1.").arg(name));
        return;
    }
    const QString outPath = QDir(destDir).filePath(name);

    // Size comes from the cached listing (no remote round-trip); it only drives
    // the progress bar and the free-space check, so an unknown size is fine --
    // the bar just stays indeterminate. Unlike the preview there is no size cap:
    // a double-click is an explicit request, so a big file gets progress and a
    // Cancel button rather than silently doing nothing.
    const FileInfo info = panel->currentEntryInfo();
    const qint64 total = (info.isValid() && info.path() == path) ? info.size() : 0;
    if (total > 0 && QStorageInfo(root).bytesAvailable() < total) {
        ttc::warning(this, tr("Open"),
                             tr("There is not enough free space in %1 to download %2.")
                                 .arg(QDir::tempPath(), name));
        return;
    }

    RemoteFetch fetch;
    fetch.name = name;
    fetch.destDir = destDir;
    fetch.total = total;
    fetch.cancel = std::make_shared<std::atomic<bool>>(false);
    fetch.onReady = std::move(onReady);
    m_remoteFetches.insert(reqId, fetch);

    // Show progress only once the download has run for half a second, so opening
    // a small file doesn't flash a dialog (same threshold as the preview pane).
    QTimer::singleShot(500, this, [this, reqId] {
        auto it = m_remoteFetches.find(reqId);
        if (it == m_remoteFetches.end() || it->dialog)
            return; // finished already, or a dialog is up
        auto *dlg = new OperationProgressDialog(this);
        dlg->setWindowTitle(tr("Open"));
        dlg->setPauseVisible(false); // a one-shot download has nothing to pause
        dlg->setDescription(tr("Downloading %1...").arg(it->name));
        dlg->setProgress(0, 1, 0, it->total, it->name);
        connect(dlg, &OperationProgressDialog::cancelRequested, this,
                [this, reqId] { cancelRemoteFetch(reqId); });
        it->dialog = dlg;
        dlg->show();
    });

    std::shared_ptr<FileProvider> provider = panel->model()->providerPtr();
    std::shared_ptr<std::atomic<bool>> cancel = fetch.cancel;
    MainWindow *self = this; // outlives the fetch: closeEvent cancels every one
    QtConcurrent::run([self, provider, path, outPath, reqId, cancel, total] {
        auto progressCb = [self, reqId](qint64 done, qint64 tot) {
            QMetaObject::invokeMethod(self, "onRemoteFetchProgress", Qt::QueuedConnection,
                                      Q_ARG(quint64, reqId), Q_ARG(qint64, done),
                                      Q_ARG(qint64, tot));
        };
        const QString local =
            downloadRemoteToTemp(provider.get(), path, outPath, cancel.get(), total, progressCb);
        QMetaObject::invokeMethod(self, "onRemoteFetchDone", Qt::QueuedConnection,
                                  Q_ARG(quint64, reqId), Q_ARG(QString, local),
                                  Q_ARG(bool, cancel->load()));
    });
}

void MainWindow::onRemoteFetchProgress(quint64 reqId, qint64 done, qint64 total) {
    auto it = m_remoteFetches.find(reqId);
    if (it == m_remoteFetches.end() || !it->dialog)
        return; // done, cancelled, or still inside the 0.5s quiet window
    it->dialog->setProgress(0, 1, done, total, it->name);
}

void MainWindow::onRemoteFetchDone(quint64 reqId, const QString &localPath, bool cancelled) {
    auto it = m_remoteFetches.find(reqId);
    if (it == m_remoteFetches.end())
        return;
    // Copy out first: onReady may well start another fetch and rehash the table.
    const RemoteFetch fetch = *it;
    m_remoteFetches.erase(it);
    if (fetch.dialog) {
        fetch.dialog->close();
        fetch.dialog->deleteLater();
    }
    if (cancelled || localPath.isEmpty()) {
        // The partial copy is already removed, so the request's own directory is
        // empty -- take it with us rather than leaving litter behind.
        if (!fetch.destDir.isEmpty())
            QDir().rmdir(fetch.destDir);
        if (cancelled)
            return; // the user asked to stop; nothing more to say
        ttc::warning(this, tr("Open"),
                             tr("Could not download %1 from the server.").arg(fetch.name));
        return;
    }
    // Hand over a read-only copy. Nothing writes it back, so an editor finding
    // it unwritable and refusing to save is the honest outcome -- the silent
    // alternative is the user's edits landing in a file that is thrown away.
    QFile::setPermissions(localPath, QFile::ReadOwner | QFile::ReadUser);
    if (fetch.onReady)
        fetch.onReady(localPath);
}

void MainWindow::cancelRemoteFetch(quint64 reqId) {
    auto it = m_remoteFetches.find(reqId);
    if (it == m_remoteFetches.end())
        return;
    if (it->cancel)
        it->cancel->store(true); // the worker's read loop aborts on the next chunk
    if (it->dialog) {
        it->dialog->close();
        it->dialog->deleteLater();
        it->dialog = nullptr;
    }
    // The entry stays until the worker reports back, which is what stops the
    // onReady callback from running for a cancelled fetch.
}

void MainWindow::runCommand(const QString &command, const QString &directory) {
    // Run through a shell so pipes, globbing, and redirection work as typed.
    const QString cwd = directory.isEmpty() && m_activePanel ? m_activePanel->currentPath()
                                                             : directory;

    // Capture output (rather than detaching) so commands that only print --
    // ls, echo, grep, or a command's error message -- are actually visible; the
    // console reveals itself only when there's output or a failure, so pure
    // side-effecting commands (mkdir, touch) stay quiet and just refresh panels.
    if (!m_commandOutput)
        m_commandOutput = new CommandOutputDialog(this);
    m_commandOutput->beginCommand(command, cwd);

    // Parented to `this`, so a launched GUI app lives for the session (not killed
    // when a local QProcess goes out of scope). Merged channels interleave
    // stdout+stderr in the order they were written, matching a terminal.
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(cwd);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        m_commandOutput->appendOutput(QString::fromLocal8Bit(proc->readAllStandardOutput()));
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            m_commandOutput->endCommand(-1, /*crashed=*/true);
            proc->deleteLater();
        }
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc](int code, QProcess::ExitStatus status) {
                m_commandOutput->appendOutput(QString::fromLocal8Bit(proc->readAllStandardOutput()));
                m_commandOutput->endCommand(code, status == QProcess::CrashExit);
                // Reflect any filesystem changes the command made.
                m_leftPanel->refresh();
                m_rightPanel->refresh();
                proc->deleteLater();
            });

    const fc::ShellInvocation shell = fc::shellInvocationFor(command);
    proc->start(shell.program, shell.arguments);
}

void MainWindow::openShortcutsDialog() {
    ShortcutsDialog dlg(m_commands.keyedOrder(), m_commands.currentSequences(),
                        m_commands.defaults(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto updated = dlg.resultShortcuts();
    for (auto it = updated.constBegin(); it != updated.constEnd(); ++it) {
        m_commands.setSequence(it.key(), it.value());
        m_settings.setShortcut(it.key(), it.value());
    }
}

void MainWindow::openMultiRenameDialog() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
    MultiRenameDialog dlg(paths, this);
    dlg.exec();
    m_activePanel->refresh();
}

void MainWindow::openSyncDialog() {
    // Pass each panel's provider, not just its path: a network tab's directory
    // is meaningless to the local filesystem, so comparing one without its
    // backend silently produced an empty result.
    SyncDialog dlg(m_leftPanel->currentPath(), m_leftPanel->model()->providerPtr(),
                   m_rightPanel->currentPath(), m_rightPanel->model()->providerPtr(), this);
    dlg.exec();
    m_leftPanel->refresh();
    m_rightPanel->refresh();
}

void MainWindow::compareSelectedFiles() {
    if (!m_activePanel)
        return;

    // "Which of these are files?" has to be asked of the backend that listed
    // them. On a network or archive tab the paths belong to the server (or to
    // the archive), so QFileInfo::isFile() rejected every one and the user was
    // told to select two files they had already selected.
    auto onlyFiles = [](FilePanel *panel) {
        QStringList files;
        FileProvider *prov = panel ? panel->model()->provider() : nullptr;
        if (!prov)
            return files;
        if (prov->isLocalFilesystem()) {
            for (const QString &p : panel->selectedPaths())
                if (QFileInfo(p).isFile())
                    files.append(p);
            return files;
        }
        for (const FileInfo &info : panel->selectedEntryInfos())
            if (info.isValid() && !info.isDir())
                files.append(info.path());
        return files;
    };

    FilePanel *const other = otherPanel(m_activePanel);
    const QStringList activeFiles = onlyFiles(m_activePanel);
    FilePanel *leftPanel = m_activePanel;
    FilePanel *rightPanel = m_activePanel;
    QString leftPath, rightPath;

    if (activeFiles.size() == 2) {
        leftPath = activeFiles.at(0);
        rightPath = activeFiles.at(1);
    } else {
        const QStringList otherFiles = onlyFiles(other);
        if (activeFiles.size() == 1 && otherFiles.size() == 1) {
            leftPath = activeFiles.first();
            rightPath = otherFiles.first();
            rightPanel = other;
        }
    }

    if (leftPath.isEmpty() || rightPath.isEmpty()) {
        ttc::information(
            this, tr("Compare by Content"),
            tr("Select two files to compare: either two in one panel, or one in each panel."));
        return;
    }

    // A diff needs the actual bytes, so each side first has to become a real
    // path on this machine: the gvfs mount when the connection has one (nothing
    // is copied), a downloaded read-only copy when it doesn't. Local paths go
    // through withLocalFile() synchronously, so a local-vs-local compare behaves
    // exactly as it always did. The dialog is still labelled with the paths the
    // user sees in the panels, not with the mount point or the temp copy.
    QPointer<FilePanel> rightGuard(rightPanel);
    withLocalFile(leftPanel, leftPath,
                  [this, rightGuard, leftPath, rightPath](const QString &leftReal) {
                      if (!rightGuard)
                          return;
                      withLocalFile(rightGuard, rightPath,
                                    [this, leftReal, leftPath, rightPath](const QString &rightReal) {
                                        auto *dlg = new CompareDialog(leftReal, rightReal, leftPath,
                                                                     rightPath, this);
                                        dlg->setAttribute(Qt::WA_DeleteOnClose);
                                        dlg->show();
                                    });
                  });
}

void MainWindow::populateFavoritesMenu(QMenu *menu, FilePanel *panel, int tabIndex) {
    const QString currentPath = panel->currentPath();
    const QStringList favorites = m_settings.favoriteDirectories();

    // 1) Bookmark / unbookmark the current directory (first item).
    if (favorites.contains(currentPath)) {
        menu->addAction(tr("Remove this directory from favorites"), this,
                        [this, currentPath]() { m_settings.removeFavoriteDirectory(currentPath); });
    } else {
        menu->addAction(tr("Bookmark this directory"), this,
                        [this, currentPath]() { m_settings.addFavoriteDirectory(currentPath); });
    }
    menu->addSeparator();

    // 2) Saved favorites (full path); clicking jumps the target tab there. When
    // opened from a tab right-click, tabIndex is that tab; otherwise it's -1 and
    // navigateTabTo() falls back to the active tab.
    if (favorites.isEmpty()) {
        QAction *placeholder = menu->addAction(tr("(No favorites yet)"));
        placeholder->setEnabled(false);
    } else {
        for (const QString &favPath : favorites)
            menu->addAction(favPath, this, [panel, favPath, tabIndex]() {
                // Favorites are local directories: drop any server this tab holds
                // and browse the local path. openLocalInTab() switches to the
                // right-clicked tab, disconnects only THAT tab, then navigates --
                // sibling tabs keep their own connections and icons.
                panel->openLocalInTab(tabIndex, favPath);
            });
    }
}

void MainWindow::openDirectoryHotlist() {
    if (!m_activePanel)
        return;
    QMenu menu(this);
    populateFavoritesMenu(&menu, m_activePanel);
    menu.exec(QCursor::pos());
}

void MainWindow::showFavoritesMenu(const QPoint &globalPos, int tabIndex) {
    if (!m_activePanel)
        return;
    QMenu menu(this);
    FilePanel *panel = m_activePanel; // favoritesMenuRequested emits panelActivated first
    // For a network tab, offer reconnect / disconnect above the favorites so the
    // user can manage the connection directly (there was no manual entry before).
    if (tabIndex >= 0 && panel->tabHasConnection(tabIndex)) {
        if (!panel->tabConnInfo(tabIndex).host.isEmpty()) {
            menu.addAction(tr("重新连接"), this,
                           [this, panel, tabIndex] { reconnectSavedTab(panel, tabIndex); });
        }
        menu.addAction(tr("断开连接"), this,
                       [panel, tabIndex] { panel->disconnectTab(tabIndex); });
        menu.addSeparator();
    }
    populateFavoritesMenu(&menu, panel, tabIndex);
    menu.exec(globalPos);
}

void MainWindow::reconnectSavedTab(FilePanel *panel, int index) {
#if FILECOMMANDER_HAS_NETWORK
    const SavedConnection c = panel->tabConnInfo(index);
    if (c.host.isEmpty())
        return;
    setupFeatureBatch();
    auto native = providerForSaved(c);
    if (!native.provider) {
        ttc::critical(this, tr("重新连接"), tr("不支持的连接类型。"));
        return;
    }
    const QString path = c.remotePath.isEmpty() ? QStringLiteral("/") : c.remotePath;
    const QString label = c.user.isEmpty() ? c.host : c.user + QLatin1Char('@') + c.host;
    panel->connectTabTo(index, native.provider, native.connectFn, path, label, c,
                        native.authFactory);
#else
    Q_UNUSED(panel)
    Q_UNUSED(index)
    ttc::information(this, tr("Reconnect"),
                     tr("Network connections are not enabled in this build."));
#endif
}


void MainWindow::setTheme(Settings::Theme theme) {
    m_settings.setTheme(theme);
    applyTheme();
}

void MainWindow::setPhosphorImages(bool on) {
    m_settings.setPhosphorImages(on);
    applyTheme();
}

void MainWindow::setPhosphorPreview(bool on) {
    m_settings.setPhosphorPreview(on);
    applyTheme();
}

void MainWindow::applyTheme() {
    m_themeManager->apply(m_settings.theme(), m_settings.phosphorImages(),
                          m_settings.phosphorPreview());
    // Restore the interface font the stylesheet swap just reset -- see the
    // matching call at the end of the constructor for what it costs to skip.
    applyInterfaceTypography();
    refreshThemedArtwork();
}

// Everything the stylesheet cannot repaint by itself: artwork that was
// RECOLOURED from the palette when it was made, and so is stale the moment the
// palette changes.
//
// Separate from applyTheme() because the startup path applies the stylesheet
// directly rather than through it, and so skipped every one of these. A panel
// is built around 480 ms and the startup theme lands around 990 ms, so the
// address row's computer glyph and the function-key bar's two glyphs spent the
// whole session in the untinted #888888 they were drawn in. Under Light and
// Dark that reads as a slightly-off grey and went unnoticed for months; under
// Green CRT it is a grey icon in a window of phosphor, which is how it was
// finally reported.
void MainWindow::refreshThemedArtwork() {
    // The memory cache holds thumbnails already tinted under the previous
    // setting, keyed by it. Dropping them costs one re-tint from the stored
    // bitmaps; the disk cache is untouched, so nothing is re-fetched or
    // re-decoded. Repaint so the panels ask for the new copies right away.
    ThumbnailCache::instance().invalidateMemoryCache();
    if (m_leftPanel)
        m_leftPanel->refreshThemeIcons();
    if (m_rightPanel)
        m_rightPanel->refreshThemeIcons();
    if (m_functionKeyBar)
        updateExtraKeyButtons();
#if FILECOMMANDER_HAS_LINUX_INTEGRATION || (defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK)
    for (ExternalConnectDialog *popup : findChildren<ExternalConnectDialog *>())
        popup->refreshThemeIcons();
#endif
    // The preview pane holds bitmaps recoloured when the file was opened, so it
    // would otherwise keep the previous treatment until the next file. Both the
    // embedded pane and any open F3 viewer window.
    if (m_quickView)
        m_quickView->refreshPhosphor();
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *viewer = qobject_cast<ViewerWindow *>(w))
            viewer->refreshPhosphor();
    }
    // Menu entries that only apply to the CRT theme.
}

void MainWindow::setLanguage(const QString &language) {
    m_settings.setLanguage(language);
    // Swap the catalog live; Qt posts QEvent::LanguageChange to this window,
    // which retranslates the UI in changeEvent(). No restart needed.
    TranslationManager::switchTo(*qApp, language);
}

void MainWindow::retranslateUi() {
    // Command labels first (fresh tr()), then everything that displays them.
    setupShortcuts();       // re-run is label-only now (shortcuts already built)
    buildTitleBarMenus();   // rebuilds Commands/View + the title bar app name
    updateFunctionKeyLabels();
    setWindowTitle(tr("FileCommander"));
    if (m_commandBar)
        m_commandBar->retranslate();

    // Column headers (and any tr()'d cell text like the type column) come from
    // the models; ask each panel to re-emit headers/cells and refresh its
    // status-bar counts.
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        if (!panel)
            continue;
        if (panel->model())
            panel->model()->retranslate();
        panel->retranslate();
    }
}

FilePanel *MainWindow::otherPanel(FilePanel *panel) const {
    return panel == m_leftPanel ? m_rightPanel : m_leftPanel;
}

bool MainWindow::promptCredentials(const QString &host, QString *user, QString *pass,
                                   const QString &error) {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("需要密码"));
    auto *form = new QFormLayout(&dlg);
    auto *info = new QLabel(
        host.isEmpty() ? tr("此连接需要用户名和密码。")
                       : tr("连接“%1”需要用户名和密码。").arg(host),
        &dlg);
    info->setWordWrap(true);
    form->addRow(info);
    // Show the server's rejection reason (e.g. wrong password) so the user knows
    // this is a retry, not a first prompt.
    if (!error.isEmpty()) {
        auto *errLabel = new QLabel(error, &dlg);
        errLabel->setWordWrap(true);
        errLabel->setProperty("semanticState", QStringLiteral("error"));
        form->addRow(errLabel);
    }
    auto *userEdit = new QLineEdit(&dlg);
    auto *passEdit = new QLineEdit(&dlg);
    passEdit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("用户名："), userEdit);
    form->addRow(tr("密码："), passEdit);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    ttc::localizeStandardButtons(box);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    userEdit->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *user = userEdit->text();
    *pass = passEdit->text();
    return true;
}

FilePanel *MainWindow::beginServerConnection() {
    setupFeatureBatch();
    // Server connections always land in a fresh tab on the LEFT panel, so they
    // never clobber whatever the (possibly right, possibly busy) active panel is
    // showing. Focus it so the user sees the new connection appear.
    FilePanel *panel = m_leftPanel;
    setActivePanel(panel);
    panel->newTab();
    panel->view()->setFocus();
    return panel;
}

void MainWindow::openServerConnectDialog(bool preselectSmb) {
#if FILECOMMANDER_HAS_NETWORK
    ConnectDialog dlg(this);
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    if (preselectSmb)
        dlg.selectProtocol(static_cast<int>(GvfsMounter::Protocol::Smb));
#else
    if (preselectSmb)
        dlg.selectProtocol(1); // GvfsMounter::Protocol::Smb, kept stable in bookmarks.
#endif
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (auto provider = dlg.remoteProvider()) {
        // Native backend: connect asynchronously on a worker thread (status line
        // shows progress), then navigate to the initial remote path -- the
        // listing waits for the connection, never freezing the UI.
        FilePanel *panel = beginServerConnection();
        panel->model()->connectNetwork(provider, dlg.connectFn(), dlg.remotePath());
        // Show the host name on the tab immediately, before it connects.
        panel->setConnectingLabel(dlg.displayLabel(), provider->scheme());
        // Wire credential retry so a wrong/blank password prompts and reconnects
        // instead of the re-entered password being silently discarded.
        if (auto af = dlg.authFactory())
            panel->model()->setAuthContext(dlg.displayLabel(), af);
        // Record for session reconnect on next launch.
        panel->setActiveTabConnInfo(dlg.connectionInfo());
        panel->navigateTo(dlg.remotePath());
    } else if (!dlg.mountedLocalPath().isEmpty()) {
        // gvfs-mounted protocols look like a local directory.
        FilePanel *panel = beginServerConnection();
        panel->navigateTo(dlg.mountedLocalPath());
    }
#else
    Q_UNUSED(preselectSmb)
    ttc::information(this, tr("Server Connection"),
                     tr("Network connections are not enabled in this build."));
#endif
}

FilePanel *MainWindow::panelShowingDir(const QString &dir) const {
    const QString target = QDir::cleanPath(dir);
    if (QDir::cleanPath(m_leftPanel->currentPath()) == target)
        return m_leftPanel;
    if (QDir::cleanPath(m_rightPanel->currentPath()) == target)
        return m_rightPanel;
    return nullptr;
}

std::shared_ptr<FileProvider> MainWindow::providerOwningPath(const QString &path) const {
    LocalFileProvider *local = LocalFileProvider::instance();
    const QString clean = QDir::cleanPath(path);
    for (FilePanel *p : {m_leftPanel, m_rightPanel}) {
        FileProvider *pv = p->model()->provider();
        if (pv == local)
            continue; // local paths are owned by the local provider
        // A remote panel owns `path` if its current directory is `path` itself or
        // an ancestor of it (so sub-folder drops into a remote pane resolve too).
        const QString root = QDir::cleanPath(p->currentPath());
        if (clean == root || clean.startsWith(root + QLatin1Char('/')))
            return p->model()->providerPtr();
    }
    return localProviderPtr();
}

std::shared_ptr<FileProvider> MainWindow::providerPtrFor(FileProvider *provider) const {
    if (!provider || provider == LocalFileProvider::instance())
        return localProviderPtr();
    for (FilePanel *p : {m_leftPanel, m_rightPanel}) {
        if (p->model()->provider() == provider)
            return p->model()->providerPtr();
    }
    return {};
}

std::shared_ptr<FileProvider> MainWindow::findLiveRemoteProvider(
    const QString &scheme, const QString &displayName) const {
    if (scheme.isEmpty() || displayName.isEmpty())
        return nullptr;
    // Match a clipboard-tagged remote source back to a still-live connection. We
    // check both panels' active providers (the dominant flow: copy in a remote
    // pane, paste while it -- or the other pane on the same server -- is still
    // open). A parked/closed connection matches nothing, so paste refuses instead
    // of silently reading a same-named local file.
    for (FilePanel *p : {m_leftPanel, m_rightPanel}) {
        FileProvider *pv = p->model()->provider();
        if (pv && pv->scheme() == scheme && pv->displayName() == displayName)
            return p->model()->providerPtr();
    }
    return nullptr;
}

QStringList MainWindow::destPathsFor(const QStringList &sources, const QString &destDir) {
    QStringList out;
    out.reserve(sources.size());
    const QDir dir(destDir);
    for (const QString &s : sources)
        out.append(dir.filePath(QFileInfo(s).fileName()));
    return out;
}

void MainWindow::handleFilesDropped(const QStringList &sources, const QString &destDir,
                                     FileListView::DropActionKind kind, FileProvider *srcProvider) {
    // Refresh + select only the affected panels: the destination (if it's one
    // of the two open panels) gets the arriving files selected, and a move also
    // removes the vanished rows from the source panel in place. A drop onto a
    // sub-folder that isn't open matches no panel and falls back to a full
    // rescan of both panels.
    FilePanel *destPanel = panelShowingDir(destDir);
    if (destPanel) {
        m_pendingDestPanel = destPanel;
        m_pendingDestPaths = destPathsFor(sources, destDir);
    }

    // Detect whether either end is a remote provider (SFTP/FTP/WebDAV/SMB). If so
    // the transfer must stream through the provider engine, exactly as F5
    // copy/move does -- treating a remote path as a local file is what made a
    // drag out of a network tab fail with a permission error.
    // Use the drag's real source provider (from the originating panel) rather than
    // guessing it from the path: an archive's virtual "/file.txt" is
    // indistinguishable from a real local path, and at an archive's "/" root the
    // ancestor guess fails outright -- which made a drag OUT of an archive read a
    // bogus local path and fail with a permission error. External drops carry no
    // source provider (null) and are plain local files.
    LocalFileProvider *local = LocalFileProvider::instance();
    std::shared_ptr<FileProvider> dstProv = providerOwningPath(destDir);
    std::shared_ptr<FileProvider> srcProv =
        srcProvider ? providerPtrFor(srcProvider) : localProviderPtr();
    if (!srcProv || !dstProv)
        return;
    const bool crossProvider = (srcProv.get() != local) || (dstProv.get() != local);

    switch (kind) {
    case FileListView::DropActionKind::Copy:
        ensureTransferProgressDialog();
        if (crossProvider)
            m_queue->enqueueProviderCopy(srcProv, sources, dstProv, destDir);
        else
            m_queue->enqueueCopy(sources, destDir);
        break;
    case FileListView::DropActionKind::Move: {
        FilePanel *srcPanel = sources.isEmpty()
                                  ? nullptr
                                  : panelShowingDir(QFileInfo(sources.first()).absolutePath());
        if (srcPanel) {
            m_pendingMovePanel = srcPanel;
            m_pendingMovePaths = sources;
        }
        ensureTransferProgressDialog();
        if (crossProvider) {
            // Cross-provider move (copy + delete source); local undo doesn't apply.
            m_queue->enqueueProviderMove(srcProv, sources, dstProv, destDir);
        } else {
            recordMoveUndo(sources, destDir);
            m_queue->enqueueMove(sources, destDir);
        }
        break;
    }
    case FileListView::DropActionKind::Link:
        if (crossProvider) {
            // Symlinks are a local-filesystem concept; can't span a remote backend.
            ttc::warning(this, tr("创建链接"),
                         tr("无法为网络位置创建符号链接。"));
        } else {
            ensureTransferProgressDialog();
            m_queue->enqueueSymlink(sources, destDir);
        }
        break;
    }
}

void MainWindow::copySelectionToClipboard() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
    QGuiApplication::clipboard()->setMimeData(
        buildFileClipboardData(paths, /*cut=*/false, m_activePanel->model()->provider()));
}

void MainWindow::cutSelectionToClipboard() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
    QGuiApplication::clipboard()->setMimeData(
        buildFileClipboardData(paths, /*cut=*/true, m_activePanel->model()->provider()));
}

void MainWindow::pasteFromClipboard() {
    if (!m_activePanel)
        return;
    if (blockArchiveWrite(m_activePanel))
        return;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime)
        return;

    QStringList sources;
    bool isCut = false;
    // Set when the clipboard carried a remote-source tag: the source paths are
    // remote and belong to this provider, so paste must NOT fall back to
    // providerOwningPath's path-ancestor guess (which could resolve to a local
    // file of the same name).
    std::shared_ptr<FileProvider> explicitSrcProv;

    if (mime->hasFormat(QLatin1String(kRemoteClipboardMime))) {
        // Our own remote tag wins: line0 cut/copy, line1 scheme, line2
        // displayName, then remote paths (kept verbatim -- they are remote, not
        // file:// URLs).
        const QList<QByteArray> lines = mime->data(QLatin1String(kRemoteClipboardMime)).split('\n');
        QString scheme, displayName;
        for (int i = 0; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (i == 0) {
                isCut = (line == "cut");
            } else if (i == 1) {
                scheme = QString::fromUtf8(line);
            } else if (i == 2) {
                displayName = QString::fromUtf8(line);
            } else if (!line.isEmpty()) {
                sources.append(QString::fromUtf8(line));
            }
        }
        explicitSrcProv = findLiveRemoteProvider(scheme, displayName);
        if (!sources.isEmpty() && !explicitSrcProv) {
            // The source connection is gone: refuse rather than silently reading a
            // same-named local file at these paths.
            ttc::warning(this, tr("粘贴"),
                         tr("源连接（%1）已关闭，无法从远端粘贴。").arg(displayName));
            return;
        }
    } else if (mime->hasFormat(QLatin1String(fc::kInternalPathsMime))) {
        // Our own copy/cut from a tab with no live connection to bind: a local
        // tab (where these are the same paths the public URLs carry) or an
        // archive tab (where the public payload deliberately carries nothing at
        // all, since an in-archive entry has no name outside this process).
        // providerOwningPath below routes them to the right backend.
        sources = fc::decodeInternalPaths(mime->data(QLatin1String(fc::kInternalPathsMime)),
                                          &isCut);
    } else if (mime->hasFormat(QStringLiteral("x-special/gnome-copied-files"))) {
        const QByteArray data = mime->data(QStringLiteral("x-special/gnome-copied-files"));
        const QList<QByteArray> lines = data.split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (line.isEmpty())
                continue;
            if (i == 0) {
                isCut = (line == "cut");
                continue;
            }
            const QUrl url(QString::fromUtf8(line));
            if (url.isLocalFile())
                sources.append(url.toLocalFile());
        }
    } else if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile())
                sources.append(url.toLocalFile());
        }
    }

    if (sources.isEmpty())
        return;

    const QString destDir = m_activePanel->currentPath();

    // Refresh only the affected panels afterwards (like copy/move/drop), never a
    // blanket rescan of both: select the arriving file(s) in the destination,
    // and for a cut also drop the moved rows from the source panel in place
    // instead of rescanning it.
    m_pendingDestPanel = m_activePanel;
    m_pendingDestPaths = destPathsFor(sources, destDir);

    // Same cross-provider routing as drag-drop / F5: if the source paths or the
    // destination belong to a remote provider, stream through the provider engine
    // instead of the local-file path (which can't reach a network location).
    LocalFileProvider *local = LocalFileProvider::instance();
    std::shared_ptr<FileProvider> dstProv = providerOwningPath(destDir);
    std::shared_ptr<FileProvider> srcProv =
        explicitSrcProv ? explicitSrcProv : providerOwningPath(sources.first());
    const bool crossProvider = (srcProv.get() != local) || (dstProv.get() != local);

    if (isCut) {
        FilePanel *srcPanel = panelShowingDir(QFileInfo(sources.first()).absolutePath());
        if (srcPanel) {
            m_pendingMovePanel = srcPanel;
            m_pendingMovePaths = sources;
        }
        ensureTransferProgressDialog();
        if (crossProvider) {
            m_queue->enqueueProviderMove(srcProv, sources, dstProv, destDir);
        } else {
            recordMoveUndo(sources, destDir);
            m_queue->enqueueMove(sources, destDir);
        }
        QGuiApplication::clipboard()->clear();
    } else if (crossProvider) {
        ensureTransferProgressDialog();
        m_queue->enqueueProviderCopy(srcProv, sources, dstProv, destDir);
    } else {
        ensureTransferProgressDialog();
        m_queue->enqueueCopy(sources, destDir);
    }
}

void MainWindow::showFileContextMenu(FilePanel *panel, const QPoint &viewPos) {
    QAbstractItemView *view = panel->activeView();
    const QModelIndex idx = view->indexAt(viewPos);
    if (idx.isValid() && !view->selectionModel()->isSelected(idx))
        view->setCurrentIndex(idx);
    setActivePanel(panel);

    const FileInfo info = panel->currentEntryInfo();
    const bool isDirectory = info.isValid() && info.isDir();
    const bool isText = !isDirectory && info.mimeType().startsWith(QStringLiteral("text/"));
    QMenu menu(this);
    auto add = [&](const QString &id, const QString &label, std::function<void()> handler = {}) {
        addCommandAction(&menu, id, label, std::move(handler));
    };

    add(QStringLiteral("open"), tr("Open"), [this] { openWithDefault(); });
    if (isDirectory) {
        add(QStringLiteral("openWith"), tr("Open With"), [this] { openWith(); });
    } else {
        // A submenu rather than a prompt: the applications registered for this
        // file's type, then everything else installed, then the file dialog --
        // which used to be the only thing on offer.
        QMenu *openWithMenu = menu.addMenu(commandText(QStringLiteral("openWith"), tr("Open With")));
        Typography::applyChromeFont(openWithMenu, m_settings);
        const QString path = panel->currentEntryPath();
        connect(openWithMenu, &QMenu::aboutToShow, this,
                [this, openWithMenu, path]() { fillOpenWithMenu(openWithMenu, path); });
    }
    if (!isDirectory) {
        if (isText)
            add(QStringLiteral("view"), tr("View"), [this] { viewCurrent(); });
        if (isText)
            add(QStringLiteral("edit"), tr("Edit"), [this] { editCurrent(); });
    }
    menu.addSeparator();
    add(QStringLiteral("copy"), tr("Copy"), [this] { copySelected(); });
    add(QStringLiteral("cutClipboard"), tr("Cut"), [this] { cutSelectionToClipboard(); });
    add(QStringLiteral("move"), tr("Move"), [this] { moveSelected(); });
    add(QStringLiteral("rename"), tr("Rename"), [this] { renameCurrent(); });
    add(QStringLiteral("delete"), tr("Delete"), [this] { deleteSelected(false); });
    add(QStringLiteral("compress"), tr("Compress"), [this] { compressSelected(); });
    // Only for something this machine can actually open. ArchiveHandler reads
    // with QFile, so on a network tab -- or inside another archive -- the
    // panel's path names something the local filesystem knows nothing about,
    // and extracting would either fail or, worse, quietly work on a same-named
    // local file. isLocalFilesystem() is the capability query written for this;
    // a displayName() test would let archive tabs through, since ArchiveProvider
    // does not override it.
    // A panel path is only a path on this machine when the backend says so, and
    // both of the submenus below hand it to something that opens it directly.
    FileProvider *provider = panel->model() ? panel->model()->provider() : nullptr;
    const bool localBackend = provider && provider->isLocalFilesystem();

    if (!isDirectory && localBackend &&
        ArchiveHandler::isSupportedArchive(panel->currentEntryPath())) {
        QMenu *extractMenu = menu.addMenu(commandText(QStringLiteral("extractTo"), tr("Extract To")));
        Typography::applyChromeFont(extractMenu, m_settings);
        addCommandAction(extractMenu, QStringLiteral("extractHere"), tr("Extract Here"),
                         [this] { extractArchiveHere(); });
        addCommandAction(extractMenu, QStringLiteral("extractToDir"),
                         tr("Extract to Folder..."), [this] { extractArchiveToDir(); });
    }

    // Only for something this platform will actually launch: an .exe on
    // Windows, an ELF binary or AppImage on Linux. The destinations differ per
    // platform too, so each entry is offered only where supports() says it is
    // real -- Startup is Windows-only, the applications menu Linux-only.
    if (!isDirectory && localBackend &&
        fc::ShellShortcuts::isLaunchable(panel->currentEntryPath())) {
        QMenu *sendMenu = menu.addMenu(commandText(QStringLiteral("sendTo"), tr("Send To")));
        Typography::applyChromeFont(sendMenu, m_settings);
        using Destination = fc::ShellShortcuts::Destination;
        if (fc::ShellShortcuts::supports(Destination::Desktop)) {
            addCommandAction(sendMenu, QStringLiteral("shortcutToDesktop"),
                             tr("Shortcut to Desktop"),
                             [this] { sendShortcutTo(Destination::Desktop); });
        }
        if (fc::ShellShortcuts::supports(Destination::Applications)) {
            addCommandAction(sendMenu, QStringLiteral("shortcutToApplications"),
                             tr("Shortcut to Applications Menu"),
                             [this] { sendShortcutTo(Destination::Applications); });
        }
        if (fc::ShellShortcuts::supports(Destination::Startup)) {
            addCommandAction(sendMenu, QStringLiteral("shortcutToStartup"), tr("Run at Startup"),
                             [this] { sendShortcutTo(Destination::Startup); });
        }
    }
    menu.addSeparator();
    if (isDirectory)
        add(QStringLiteral("calcSize"), tr("Calculate Folder Size"));
    add(QStringLiteral("copyPath"), tr("Copy Path"), [panel]() {
        const QStringList paths = panel->selectedPaths();
        if (!paths.isEmpty())
            QGuiApplication::clipboard()->setText(paths.join('\n'));
    });
    add(QStringLiteral("properties"), tr("Properties"));
    menu.exec(view->viewport()->mapToGlobal(viewPos));
}

void MainWindow::showBlankContextMenu(FilePanel *panel, const QPoint &viewPos) {
    setActivePanel(panel);
    QMenu menu(this);
    menu.addAction(tr("Paste"), this, &MainWindow::pasteFromClipboard);
    menu.addSeparator();
    menu.addAction(tr("New Folder"), this, &MainWindow::makeDirectory);
    menu.addSeparator();
    menu.addAction(tr("Open Terminal Here"), this, &MainWindow::openTerminalHere);
    menu.addAction(tr("Refresh"), this, &MainWindow::refreshActivePanel);
    menu.exec(panel->activeView()->viewport()->mapToGlobal(viewPos));
}

void MainWindow::setActivePanel(FilePanel *panel) {
    m_activePanel = panel;
    if (panel) {
        m_commandBar->setDirectory(panel->currentPath());
        panel->setActive(true);
        if (FilePanel *other = otherPanel(panel))
            other->setActive(false);
    }
}

bool MainWindow::currentEntryIsDir() const {
    if (!m_activePanel)
        return false;
    const QString path = m_activePanel->currentEntryPath();
    // The backend's own listing is the only thing that knows. QFileInfo asks
    // THIS filesystem about a name that lives on a server (or inside an
    // archive), where it answers "not a directory" for everything -- including
    // directories -- and occasionally answers about a local file that happens to
    // share the name.
    const FileInfo info = m_activePanel->currentEntryInfo();
    if (info.isValid() && info.path() == path)
        return info.isDir();
    return QFileInfo(path).isDir();
}

void MainWindow::viewCurrent() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || currentEntryIsDir())
        return;
    // F3 opens the shared preview widget in a top-level window -- same UI and
    // capabilities as the Ctrl+Q pane (image/text/video/PDF/markdown/office,
    // and a read-only archive listing). Archives are still browsed in-panel by
    // double-clicking into them, and extracted from the context menu.
    //
    // ViewerWindow reads with QFile, so it needs a path that exists on this
    // machine. It used to be handed the provider's path straight through, which
    // on a network or archive tab is why F3 opened an empty window.
    QPointer<FilePanel> guard(m_activePanel);
    resolveRealPath(m_activePanel, path, [this, guard, path](const QString &real) {
        if (!real.isEmpty()) {
            (new ViewerWindow(m_settings, real, this))->show();
            return;
        }
        if (!guard)
            return;
        // No mount to read through. Viewing is read-only anyway, so a temp copy
        // loses nothing here -- unlike F4, where it would be a dead end.
        fetchRemoteCopy(guard, path, [this](const QString &localPath) {
            (new ViewerWindow(m_settings, localPath, this))->show();
        });
    });
}

// A .tar.gz is two containers deep, and a few packaging habits add another; well
// past that, a chain this long is a malformed or hostile file rather than
// something a user meant to unpack, and the loop must not follow it forever.
static constexpr int kMaxNestedExtractions = 16;

void MainWindow::smartExtractArchive(const QString &archivePath, const QString &destDir) {
    QString source = archivePath;
    QString base = destDir;
    QString finalDir;
    // Carried between levels: nested archives are very often encrypted with the
    // same password as their wrapper, so try it before asking again. If it does
    // not fit, the attempt costs one listing pass and the prompt follows.
    QString passphrase;
    bool promptedForThisArchive = false;
    int depth = 0;

    for (;;) {
        QString err;
        const ArchiveHandler::SmartResult res =
            ArchiveHandler::smartExtract(source, base, passphrase, &err);

        // Encrypted: ask, then retry the SAME archive. Nothing has been written
        // yet -- the encryption was found while listing -- so this is a retry,
        // not a partial extraction being resumed.
        if (res.status == ArchiveHandler::Status::NeedPassword ||
            res.status == ArchiveHandler::Status::WrongPassword) {
            const QString name = QFileInfo(source).fileName();
            // "Wrong password" only once the user has actually typed one for
            // this archive. A carried-over password that does not fit is not
            // the user's mistake, so it asks plainly instead of accusing.
            const bool wrong =
                promptedForThisArchive && res.status == ArchiveHandler::Status::WrongPassword;
            bool ok = false;
            const QString entered = ttc::getText(
                this, tr("Password required"),
                wrong ? tr("Incorrect password. Try again for “%1”:").arg(name)
                      : tr("“%1” is encrypted. Enter its password:").arg(name),
                QLineEdit::Password, QString(), &ok);
            if (!ok || entered.isEmpty())
                break; // Cancelled: keep whatever earlier levels produced.
            passphrase = entered;
            promptedForThisArchive = true;
            continue;
        }
        if (res.status == ArchiveHandler::Status::EncryptedUnsupported) {
            ttc::warning(this, tr("Extract"),
                         tr("“%1” uses an encryption this build cannot read.")
                             .arg(QFileInfo(source).fileName()));
            break;
        }
        if (!res.ok) {
            // A failure on an inner level still leaves the outer levels
            // extracted, so report it without discarding what worked.
            ttc::warning(this, tr("Extract"), tr("Extraction failed: %1").arg(err));
            if (depth == 0)
                return;
            break;
        }

        finalDir = res.finalDir;
        if (res.nestedArchivePath.isEmpty())
            break;

        if (++depth >= kMaxNestedExtractions) {
            ttc::warning(this, tr("Extract"),
                         tr("Stopped after %1 nested archives; the innermost one was left "
                            "packed.")
                             .arg(depth));
            break;
        }

        source = res.nestedArchivePath;
        base = QFileInfo(res.nestedArchivePath).absolutePath();
        promptedForThisArchive = false; // a new archive, so a new prompt is not a retry
    }

    // Cancelled at the very first password prompt: nothing was written, so
    // saying where it landed would be a lie.
    if (finalDir.isEmpty())
        return;

    // Refresh whichever panel is showing the destination so the new files appear.
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        if (panel && panel->currentPath() == destDir)
            panel->refresh();
    }
    // The count matters when it is not 1: recursion is automatic now, so this is
    // the only place the user learns how far it went.
    ttc::information(this, tr("Extract"),
                     depth > 0 ? tr("Extracted %1 nested archives to %2").arg(depth + 1).arg(finalDir)
                               : tr("Extracted archive to %1").arg(finalDir));
}

// True when the active panel's current row is an archive this machine can read.
//
// The locality test is the point. ArchiveHandler opens with QFile, so a path
// from a network tab, or from inside another archive, is not something it can
// open -- and a same-named local file would be operated on instead of failing
// cleanly. isLocalFilesystem() answers that directly; inferring it from
// displayName() being empty would wave archive tabs through, because
// ArchiveProvider does not override displayName().
bool MainWindow::currentEntryIsExtractableArchive() const {
    if (!m_activePanel)
        return false;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || !ArchiveHandler::isSupportedArchive(path))
        return false;
    FileProvider *provider = m_activePanel->model() ? m_activePanel->model()->provider() : nullptr;
    return provider && provider->isLocalFilesystem();
}

void MainWindow::sendShortcutTo(fc::ShellShortcuts::Destination where) {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (!fc::ShellShortcuts::isLaunchable(path))
        return;
    FileProvider *provider = m_activePanel->model() ? m_activePanel->model()->provider() : nullptr;
    if (!provider || !provider->isLocalFilesystem())
        return; // repeated here because this is reachable without the menu

    const PlatformResult res = fc::ShellShortcuts::create(path, where);
    if (!res.ok) {
        ttc::warning(this, tr("Send To"),
                     tr("Could not create the shortcut: %1").arg(res.message));
        return;
    }
    // Names the folder for the two destinations the user cannot simply look at:
    // a startup entry they will want to undo later, and a menu entry that does
    // not appear anywhere they were already looking.
    const QString name = QFileInfo(path).completeBaseName();
    switch (where) {
    case fc::ShellShortcuts::Destination::Startup:
        ttc::information(this, tr("Send To"),
                         tr("“%1” will start at sign-in.\nRemove it from:\n%2")
                             .arg(name, fc::ShellShortcuts::locationFor(where)));
        break;
    case fc::ShellShortcuts::Destination::Applications:
        ttc::information(this, tr("Send To"),
                         tr("“%1” was added to the applications menu.").arg(name));
        break;
    case fc::ShellShortcuts::Destination::Desktop:
        ttc::information(this, tr("Send To"), tr("Shortcut created on the desktop."));
        break;
    }
}

// Offers to add the execute bit to an AppImage that lacks it, and reports
// whether the file is runnable afterwards.
//
// This is the state a browser download leaves an AppImage in, and it is the
// single most common reason "nothing happens when I run it" -- the format's
// worst first impression. Asked rather than done silently: adding an execute
// bit is a change to a file the user did not ask to modify.
bool MainWindow::offerExecutableBit(const QString &path) {
    if (!fc::ShellShortcuts::needsExecutableBit(path))
        return true; // already runnable, or not the case this handles

    const auto answer = ttc::question(
        this, tr("Not executable"),
        tr("“%1” is an AppImage but is not marked executable, so it cannot run.\n\n"
           "Add the execute permission now?")
            .arg(QFileInfo(path).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return false;

    const PlatformResult res = fc::ShellShortcuts::makeExecutable(path);
    if (!res.ok) {
        ttc::warning(this, tr("Not executable"),
                     tr("Could not add the execute permission: %1").arg(res.message));
        return false;
    }
    m_activePanel->refresh(); // the permissions column is now wrong
    return true;
}

void MainWindow::extractArchiveHere() {
    if (!currentEntryIsExtractableArchive())
        return;
    smartExtractArchive(m_activePanel->currentEntryPath(), m_activePanel->currentPath());
}

void MainWindow::extractArchiveToDir() {
    if (!currentEntryIsExtractableArchive())
        return;
    const QString path = m_activePanel->currentEntryPath();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Extract to"), otherPanel(m_activePanel)->currentPath());
    if (!dir.isEmpty())
        smartExtractArchive(path, dir);
}

void MainWindow::editCurrent() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || currentEntryIsDir())
        return;

    if (ImageViewer::isImage(path)) {
        ttc::information(this, tr("Edit"),
                                  tr("Image files can't be edited; use F3 to view."));
        return;
    }

    // Inside an archive there is nothing to edit in place and no mount that
    // would change that, so say the thing that is actually true there ("copy it
    // out") rather than the connection advice below.
    if (blockArchiveWrite(m_activePanel))
        return;

    // Editing needs a path that is not only readable but WRITABLE, and that is
    // exactly what a gvfs mount provides and a downloaded copy does not: Save
    // goes back to the server. F4 used to hand TextEditor the provider's path,
    // which QFile could not open, and the failure below then swallowed it -- so
    // on a network tab F4 did nothing at all, with no message.
    const QString name = QFileInfo(path).fileName();
    resolveRealPath(m_activePanel, path, [this, name](const QString &real) {
        if (real.isEmpty()) {
            // Deliberately NOT a downloaded copy. The copy is read-only, and
            // even if it were not, Save would write to a temp file the user
            // never sees again -- losing the edit while looking like it worked.
            ttc::warning(this, tr("Edit"),
                         tr("%1 cannot be edited in place.\n\nEditing a file on this "
                            "connection needs it mounted through GVfs (the gvfs-backends "
                            "package). Copy the file to a local folder to edit it.")
                             .arg(name));
            return;
        }
        // Probe first: loadFile() announces the one refusal it knows about (the
        // 50 MB cap) and returns false silently for everything else, so the
        // unreadable case gets its own message here without doubling up on that
        // one. A mount that died since it was resolved lands here.
        QFile probe(real);
        if (!probe.open(QIODevice::ReadOnly)) {
            ttc::warning(this, tr("Edit"),
                         tr("Could not open %1 for editing: %2").arg(name, probe.errorString()));
            return;
        }
        probe.close();
        auto *editor = new TextEditor();
        if (!editor->loadFile(real)) {
            delete editor; // loadFile already said why
            return;
        }
        editor->resize(900, 700);
        editor->show();
    });
}

// A flat search listing spans many directories, so "create it here" has no
// answer. Saying so beats currentPath()'s fallback, which is a real directory
// and therefore creates the thing somewhere the user cannot see it.
bool MainWindow::blockWithoutWorkingDirectory(FilePanel *panel, const QString &title) {
    if (!panel || panel->hasWorkingDirectory())
        return false;
    ttc::information(this, title,
                     tr("This tab lists results from several directories, so there is no "
                        "single folder to create it in. Open one of the results' folders "
                        "first."));
    return true;
}

bool MainWindow::blockArchiveWrite(FilePanel *panel) {
    if (!panel || !panel->isArchive())
        return false;
    ttc::information(this, tr("Read-only"),
                             tr("This archive is read-only. Copy files out to a folder to "
                                "modify them."));
    return true;
}

void MainWindow::copySelected() {
    if (!m_activePanel)
        return;
    const QStringList sources = m_activePanel->selectedPaths();
    if (sources.isEmpty())
        return;
    FilePanel *dest = otherPanel(m_activePanel);
    // Copying OUT of an archive is fine; copying INTO one is not (read-only).
    if (blockArchiveWrite(dest))
        return;
    const QString destDir = dest->currentPath();

    // Refresh only the destination panel afterwards and select the arriving
    // file(s); the source panel is left exactly as it is (nothing left it).
    m_pendingDestPanel = dest;
    m_pendingDestPaths = destPathsFor(sources, destDir);

    // When either panel is backed by a remote provider (e.g. SFTP), stream the
    // transfer through the provider engine (handles resume + progress) rather
    // than the local QFile path, which only understands the local filesystem.
    std::shared_ptr<FileProvider> srcProv = m_activePanel->model()->providerPtr();
    std::shared_ptr<FileProvider> dstProv = dest->model()->providerPtr();
    LocalFileProvider *local = LocalFileProvider::instance();
    if (srcProv.get() != local || dstProv.get() != local) {
        ensureTransferProgressDialog();
        m_queue->enqueueProviderCopy(srcProv, sources, dstProv, destDir);
        return;
    }

    // Copying a single item into the same directory it lives in: ask for the
    // new name (TC's F5-in-place behaviour) instead of silently duplicating.
    if (sources.size() == 1 &&
        QDir::cleanPath(destDir) == QDir::cleanPath(m_activePanel->currentPath())) {
        const QFileInfo fi(sources.first());
        bool ok = false;
        const QString newName = ttc::getText(this, tr("Copy"), tr("Copy to:"),
                                                       QLineEdit::Normal, fi.fileName(), &ok);
        if (!ok || newName.isEmpty()) {
            m_pendingDestPanel = nullptr; // cancelled: no post-copy refresh plan
            m_pendingDestPaths.clear();
            return;
        }
        const QString target = QDir(destDir).filePath(newName);
        m_pendingDestPaths = QStringList{target};
        ensureTransferProgressDialog();
        m_queue->enqueueCopyAs(sources.first(), target);
        return;
    }
    ensureTransferProgressDialog();
    m_queue->enqueueCopy(sources, destDir);
}

void MainWindow::moveSelected() {
    if (!m_activePanel)
        return;
    // Move = copy + delete-source; neither source nor dest may be a read-only
    // archive.
    if (blockArchiveWrite(m_activePanel) || blockArchiveWrite(otherPanel(m_activePanel)))
        return;
    const QStringList sources = m_activePanel->selectedPaths();
    if (sources.isEmpty())
        return;
    FilePanel *dest = otherPanel(m_activePanel);
    const QString destDir = dest->currentPath();

    // Remove the moved rows from the source panel in place (like a delete) and
    // refresh only the destination panel, selecting the arriving file(s).
    m_pendingMovePanel = m_activePanel;
    m_pendingMovePaths = sources;
    m_pendingDestPanel = dest;
    m_pendingDestPaths = destPathsFor(sources, destDir);

    std::shared_ptr<FileProvider> srcProv = m_activePanel->model()->providerPtr();
    std::shared_ptr<FileProvider> dstProv = dest->model()->providerPtr();
    LocalFileProvider *local = LocalFileProvider::instance();
    ensureTransferProgressDialog();
    if (srcProv.get() != local || dstProv.get() != local) {
        // Cross-provider move (copy + delete source); local undo doesn't apply.
        m_queue->enqueueProviderMove(srcProv, sources, dstProv, destDir);
        return;
    }

    recordMoveUndo(sources, destDir);
    m_queue->enqueueMove(sources, destDir);
}

void MainWindow::makeDirectory() {
    if (!m_activePanel)
        return;
    if (blockArchiveWrite(m_activePanel))
        return;
    if (blockWithoutWorkingDirectory(m_activePanel, tr("New Folder")))
        return;
    bool ok = false;
    const QString name =
        ttc::getText(this, tr("New Folder"), tr("Folder name:"), QLineEdit::Normal,
                               QString(), &ok);
    if (ok && !name.isEmpty()) {
        // Refresh this panel afterwards and select the freshly-created folder
        // (same select-after-reload mechanism used by copy/move).
        m_pendingDestPanel = m_activePanel;
        const QString parent = m_activePanel->currentPath();
        m_pendingDestPaths = QStringList{QDir(parent).filePath(name)};
        // On a network tab go through the provider so the folder is created on
        // the remote host; a plain enqueueMkdir would hit the local filesystem.
        std::shared_ptr<FileProvider> prov = m_activePanel->model()->providerPtr();
        ensureTransferProgressDialog();
        if (prov.get() != LocalFileProvider::instance())
            m_queue->enqueueProviderMkdir(prov, parent, name);
        else
            m_queue->enqueueMkdir(parent, name);
    }
}

void MainWindow::deleteSelected(bool permanent) {
    if (!m_activePanel)
        return;
    if (blockArchiveWrite(m_activePanel))
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;

    // On a network tab, delete goes through the provider (recursively) on the
    // remote host. There is no trash remotely, so it is always permanent.
    std::shared_ptr<FileProvider> prov = m_activePanel->model()->providerPtr();
    const bool remote = prov.get() != LocalFileProvider::instance();
    const bool goesPermanent = permanent || remote;

    // Trash deletes can skip the prompt when the user turned confirmation off
    // (Config menu). Permanent deletes (Shift+Del, or any remote delete) always
    // confirm -- they can't be undone from the trash.
    if (goesPermanent || m_settings.confirmDelete()) {
        // The dialog measures selected folders itself. It has to: the listing
        // knows a directory entry's own size, which is not the size of what is
        // about to be deleted, so a selection of folders used to be offered for
        // deletion as "0 bytes".
        const DeleteSelectionSummary summary =
            summarizeDeleteSelection(m_activePanel->model(), paths);
        // `remote` is true for anything that is not this filesystem, archive
        // tabs included, so it is also the right test for "can we walk it".
        const bool measureLocally = !remote;
        if (!DeleteConfirmDialog::ask(this, paths, summary, goesPermanent, measureLocally))
            return;
    }
    // Remember what we're deleting (and where) so the queue-finished handler
    // removes exactly these rows and selects the next file without a full rescan.
    m_pendingDeletePanel = m_activePanel;
    m_pendingDeletePaths = paths;
    ensureTransferProgressDialog();
    if (remote)
        m_queue->enqueueProviderDelete(prov, paths);
    else
        m_queue->enqueueDelete(paths, /*toTrash=*/!permanent);
}

void MainWindow::compressSelected() {
    if (!m_activePanel)
        return;
    const QStringList sources = m_activePanel->selectedPaths();
    if (sources.isEmpty())
        return;

    const QString destDir = otherPanel(m_activePanel)->currentPath();
    const QString defaultName = QFileInfo(sources.first()).completeBaseName();

    CompressDialog dlg(destDir, defaultName, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    QString err;
    if (!ArchiveHandler::create(dlg.archivePath(), sources, dlg.format(), dlg.passphrase(),
                                 dlg.encryptHeaders(), dlg.compressionLevel(), &err)) {
        ttc::warning(this, tr("Compress"), tr("Compression failed: %1").arg(err));
        return;
    }
    otherPanel(m_activePanel)->refresh();
}

void MainWindow::openSearch() {
    if (!m_activePanel)
        return;
    FilePanel *panel = m_activePanel;
    FileProvider *prov = panel->model()->provider();
    const bool network = prov && !prov->displayName().isEmpty();
    // A network tab browses provider-internal paths (an SMB share's "/share/docs"),
    // which name nothing on this machine -- searching them with QDirIterator finds
    // exactly zero files. Hand the dialog the backend so it walks that instead.
    // Sharing ownership is what keeps it valid: the search is asynchronous and
    // the tab may be closed or reconnected before it ends.
    std::shared_ptr<FileProvider> searchProvider =
        network ? panel->model()->providerPtr() : nullptr;
    auto *dlg = new SearchDialog(panel->currentPath(), searchProvider, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &SearchDialog::navigateRequested, this,
            [panel, searchProvider](const QString &path, bool isDir) {
                if (isDir) {
                    panel->navigateTo(path);
                    return;
                }
                // Show the file in its own directory. parentPath() is pure string
                // work in every backend (no lock, no round-trip), so calling it
                // here on the GUI thread is safe.
                panel->navigateTo(searchProvider ? searchProvider->parentPath(path)
                                                 : QFileInfo(path).absolutePath());
            });
    // "Send to panel": open every result as a flat cross-directory listing in a
    // NEW tab of the active panel, titled after the search keyword.
    connect(dlg, &SearchDialog::feedToPanelRequested, this,
            [this](const QString &keyword, const QStringList &paths) {
                if (m_activePanel)
                    m_activePanel->showSearchResultsInNewTab(keyword, paths);
            });
    dlg->show();
}

void MainWindow::renameCurrent() {
    if (!m_activePanel)
        return;
    if (blockArchiveWrite(m_activePanel))
        return;
    // In-place cell editing (like TC/Explorer/Nautilus) rather than a
    // modal dialog: FileSystemModel::flags()/setData() do the actual
    // rename synchronously when editing commits. NoEditTriggers stays set
    // on both views globally, so only this explicit edit() call (never a
    // stray click) can start it. beginRenameCurrent() acts on whichever view
    // (list or thumbnail) is currently showing.
    m_activePanel->beginRenameCurrent();
}

void MainWindow::navigateBack() {
    if (m_activePanel)
        m_activePanel->goBack();
}

void MainWindow::navigateForward() {
    if (m_activePanel)
        m_activePanel->goForward();
}

void MainWindow::navigateUp() {
    if (m_activePanel)
        m_activePanel->navigateUp();
}

void MainWindow::refreshActivePanel() {
    if (!m_activePanel)
        return;
    // In the computer view there is no directory to re-scan: the rows are a
    // snapshot of what was plugged in and reachable, so a plain refresh would
    // re-list exactly the same stale set. Rebuild it instead, which is what the
    // user is asking for by pressing refresh there.
    if (m_activePanel->isComputerView()) {
        showComputerView(m_activePanel);
        return;
    }
    m_activePanel->refresh();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // Stop any in-flight "open a network file" download before the window that
    // its worker reports back to goes away; the read loop checks the flag on
    // every chunk. The downloaded copies themselves are left on disk on purpose
    // (see ensureOpenTempDir).
    for (auto it = m_remoteFetches.begin(); it != m_remoteFetches.end(); ++it)
        if (it->cancel)
            it->cancel->store(true);
    // Downloaded archives, unlike those copies, ARE ours to clean up: nothing
    // outside this process reads them. Destroying the QTemporaryDir sweeps up any
    // whose browse the user never stepped out of.
    m_scratch.discardArchive();

    m_settings.setWindowGeometry(saveGeometry());
    // Persist each panel's column layout independently: base widths + hidden
    // mask + sort (replaces the old single left-panel blob shared to both).
    auto savePanelColumns = [this](FilePanel *panel, const QString &side) {
        FileListView *view = panel->view();
        QHeaderView *header = view->horizontalHeader();
        QStringList parts;
        const QVector<int> widths = view->columnBaseWidths();
        for (int w : widths)
            parts << QString::number(w);
        m_settings.setColumnBaseWidths(side, parts.join(QLatin1Char(',')));
        int mask = 0;
        for (int c = 0; c < header->count(); ++c)
            if (header->isSectionHidden(c))
                mask |= (1 << c);
        m_settings.setHiddenColumnsMask(side, mask);
        m_settings.setSortColumn(side, view->sortColumn());
        m_settings.setSortOrder(side, static_cast<int>(view->sortOrder()));
    };
    savePanelColumns(m_leftPanel, QStringLiteral("left"));
    savePanelColumns(m_rightPanel, QStringLiteral("right"));
    // Persist the panel divider position.
    m_settings.setPanelSplitterState(m_panelSplitter->saveState());

    auto snapshotPanel = [](FilePanel *panel) {
        SessionPanelData data;
        const auto snap = panel->tabSnapshot();
        for (int i = 0; i < snap.size(); ++i) {
            SessionTabData t;
            t.path = snap.at(i).path;
            t.selectedFiles = snap.at(i).selectedFiles;
            t.computerView = snap.at(i).computerView;
            t.conn = panel->tabConnInfo(i); // network reconnect descriptor (host empty => local)
            data.tabs.append(t);
        }
        data.activeTab = panel->activeTabIndex();
        return data;
    };
    SessionPanelData leftSession = snapshotPanel(m_leftPanel);
    SessionPanelData rightSession = snapshotPanel(m_rightPanel);
    SessionManager::save(leftSession, rightSession);

    QMainWindow::closeEvent(event);

    // MainWindow is the only window that should keep the app alive, but
    // other top-level widgets exist (e.g. m_progressDialog, constructed up
    // front and only shown conditionally) that can otherwise prevent Qt's
    // quitOnLastWindowClosed from firing reliably. Quit explicitly rather
    // than depend on that heuristic.
    qApp->quit();
}
