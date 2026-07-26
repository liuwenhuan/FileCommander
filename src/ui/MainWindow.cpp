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
#include <functional>
#include <QLabel>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QWidgetAction>
#include <QTreeWidget>
#include <QFileDialog>
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

#include <QX11Info>     // Qt5::X11Extras: xcb connection for the opaque-region hint
#include <xcb/xcb.h>
#include <cstdlib>      // free() for xcb replies

#include "ArchiveHandler.h"
#include "CommandBar.h"
#include "TranslationManager.h"
#include "CompressDialog.h"
#include "FilePanel.h"
#include "IconFileView.h"
#include "FileSplitter.h"
#include "MpvStreamSource.h"
#include "QuickView.h"
#include "ThumbnailCache.h"
#include "ViewerWindow.h"
#include "FileListView.h"
#include "FileSystemModel.h"
#include "ExternalPaths.h"
#include "LocalFileProvider.h"
#include "FunctionKeyBar.h"
#include "ImageViewer.h"
#include "OperationQueue.h"
#include "SearchDialog.h"
#include "SessionManager.h"
#include "TitleBar.h"
#include "dialogs/ChecksumDialog.h"
#include "dialogs/CommandOutputDialog.h"
#include "dialogs/ConnectDialog.h"
#include "dialogs/SecureWipeDialog.h"

#include <unistd.h> // getuid() for the gvfs mount path
#include "Settings.h"
#include "ThemeManager.h"
#include "TextEditor.h"
#include "dialogs/CompareDialog.h"
#include "dialogs/MultiRenameDialog.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/TransferProgressDialog.h"
#include "dialogs/OverwriteConfirmDialog.h"
#include "dialogs/PropertiesDialog.h"
#include "dialogs/ShortcutsDialog.h"
#include "dialogs/SyncDialog.h"

// Feature batch: external-connection picker, quick notepad, online update.
#include "NotepadPanel.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/ExternalConnectDialog.h"
#include "dialogs/UpdateDialog.h"
#include "devices/RemovableDeviceMonitor.h"
#include "tree/NetworkTreeRegistry.h"
#include "network/ConnectionStore.h"
#include "network/CurlFtpProvider.h"
#include "network/CurlWebDavProvider.h"
#include "network/GvfsMounter.h"
#include "network/SftpProvider.h"
#include "network/SmbHostBrowser.h"
#include "network/SmbProvider.h"
#include "update/UpdateChecker.h"

#include <QApplication>
#include <QDate>
#include <memory>

namespace {
// A splitter whose handle paints its own grey line across the full panel
// height (tabs, breadcrumb, list, status bar). The deepin (DTK) style ignores
// the handle's palette/autoFillBackground, so we paint it ourselves.
class PaintedHandle : public QSplitterHandle {
public:
    PaintedHandle(Qt::Orientation o, QSplitter *parent) : QSplitterHandle(o, parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x8a, 0x8a, 0x8a));
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
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
    m_themeManager->apply(m_settings.theme());

    // A self-painted splitter so the divider line runs the full panel height
    // -- up through the breadcrumb and tab row -- clearly separating the two
    // panels. (A stylesheet on the splitter would cascade onto the tables and
    // slow their repaints, so we paint just the handle.)
    auto *splitter = new PanelSplitter(this);
    m_panelSplitter = splitter;
    m_leftPanel = new FilePanel(splitter);
    m_rightPanel = new FilePanel(splitter);
    splitter->addWidget(m_leftPanel);
    splitter->addWidget(m_rightPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setHandleWidth(2);
    m_quickView = new QuickView(m_settings, QuickView::Context::Embedded, this);
    m_quickView->hide(); // parked until Ctrl+Q swaps it into a panel slot
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
    m_queue->setConflictHandler(
        [this](const FileConflict &conflict) { return OverwriteConfirmDialog::ask(this, conflict); });
    m_queue->setErrorHandler([this](const QString &path, const QString &error) {
        // Custom-button message box (Retry / Skip / Skip All / Cancel), embedded
        // in the themed frameless chrome like the ttc::message() wrappers.
        FramelessDialog dlg(this);
        dlg.setWindowTitle(tr("Operation Error"));
        auto *box = new QMessageBox(QMessageBox::Warning, tr("Operation Error"),
                                    tr("%1\n\n%2").arg(error, path), QMessageBox::NoButton, &dlg);
        box->setWindowFlags(Qt::Widget);
        QPushButton *retry = box->addButton(tr("Retry"), QMessageBox::AcceptRole);
        QPushButton *skip = box->addButton(tr("Skip"), QMessageBox::RejectRole);
        QPushButton *skipAll = box->addButton(tr("Skip All"), QMessageBox::RejectRole);
        box->addButton(tr("Cancel"), QMessageBox::DestructiveRole);
        connect(box, &QMessageBox::finished, &dlg, [&dlg](int) { dlg.accept(); });
        auto *layout = new QVBoxLayout(&dlg);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(box);
        dlg.exec();
        if (box->clickedButton() == retry)
            return ErrorAction::Retry;
        if (box->clickedButton() == skip)
            return ErrorAction::Skip;
        if (box->clickedButton() == skipAll)
            return ErrorAction::SkipAll;
        return ErrorAction::Cancel;
    });
    // Self-wiring progress dialog: shows itself on started, tracks bytes/speed/ETA
    // + queue depth, and drives cancel/pause/resume. It covers local operations
    // and (with speed/ETA) large remote SFTP/FTP/WebDAV transfers.
    m_progressDialog = new TransferProgressDialog(m_queue, this);
    connect(m_queue, &OperationQueue::started, this,
            [this](const QString &) { m_operationErrors.clear(); });
    connect(m_queue, &OperationQueue::finished, this, [this](bool) {
        bool handledPlan = false;

        if (m_pendingDeletePanel) {
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

        if (m_pendingMovePanel) {
            // A move just finished: the source files vanished, so the source
            // panel settles the same way a delete does.
            FilePanel *panel = m_pendingMovePanel;
            m_pendingMovePanel = nullptr;
            panel->settleAfterRemoval(m_pendingMovePaths);
            m_pendingMovePaths.clear();
            handledPlan = true;
        }

        if (m_pendingDestPanel) {
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

    // Optional bars + splitter layout. Applied before buildTitleBarMenus() so
    // the View-menu checkmarks (which read the widgets' visibility) match.
    m_commandBar->setVisible(m_settings.showCommandBar());
    m_functionKeyBar->setVisible(m_settings.showFunctionKeyBar());
    if (const QByteArray s = m_settings.panelSplitterState(); !s.isEmpty())
        m_panelSplitter->restoreState(s);

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    SessionPanelData leftSession, rightSession;
    if (SessionManager::load(leftSession, rightSession)) {
        // Network tabs are not restored at all -- reconnecting during startup
        // blocks on an unreachable server or pops a password dialog before the
        // window is even usable. See SessionManager::dropNetworkTabs().
        SessionManager::dropNetworkTabs(leftSession);
        SessionManager::dropNetworkTabs(rightSession);
        // Drop tabs whose removable medium is gone (device unplugged while the
        // app was closed), keeping the active-tab index pointing at a survivor.
        auto buildTabs = [](const SessionPanelData &s, int &activeOut) {
            QVector<QPair<QString, QStringList>> tabs;
            int active = 0;
            for (int i = 0; i < s.tabs.size(); ++i) {
                if (isMissingRemovablePath(s.tabs.at(i).path))
                    continue;
                if (i < s.activeTab)
                    ++active; // this kept tab sits before the old active one
                tabs.append({s.tabs.at(i).path, s.tabs.at(i).selectedFiles});
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
    } else {
        m_leftPanel->navigateTo(home);
        m_rightPanel->navigateTo(home);
    }

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
                    if (FileProvider *prov = model->provider())
                        m_queue->enqueueProviderRename(prov, oldPath, newName);
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
        connect(panel, &FilePanel::pathChanged, this, [this, panel](const QString &path) {
            if (panel == m_activePanel)
                m_commandBar->setDirectory(path);
        });
        connect(panel->view(), &FileListView::filesDropped, this, &MainWindow::handleFilesDropped);
        // The thumbnail view supports the same drag-and-drop; route it through
        // the same handler (its filesDropped signature matches FileListView's).
        connect(panel->iconView(), &IconFileView::filesDropped, this,
                &MainWindow::handleFilesDropped);
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
                    m_lastUndo.provider = (prov == LocalFileProvider::instance()) ? nullptr : prov;
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
        panel->iconView()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(panel->iconView(), &QWidget::customContextMenuRequested, this,
                [this, panel](const QPoint &pos) {
                    setActivePanel(panel);
                    if (panel->iconView()->indexAt(pos).isValid())
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
    }

    setActivePanel(m_leftPanel);
    m_leftPanel->view()->setFocus();
    setTabOrder(m_leftPanel->view(), m_rightPanel->view());
    setTabOrder(m_rightPanel->view(), m_leftPanel->view());

    buildTitleBarMenus();
    setupShortcuts();

    // Apply the persisted file-list font size now that both panels and the
    // View-menu control exist.
    const int listFont = m_settings.listFontSize();
    m_leftPanel->setListFontSize(listFont);
    m_rightPanel->setListFontSize(listFont);
    if (m_quickView)
        m_quickView->setContentFontSize(listFont);

    // Per-side, per-mode view scale (status-bar -/+ buttons): restore whatever
    // was last saved, then persist again whenever a panel's -/+ click changes
    // it. Applied after setListFontSize() above since it can override the
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
}

void MainWindow::buildTitleBarMenus() {
    // Re-runnable (called again on a live language change): drop the previous
    // menus so we don't leak them. setMenuWidget() deletes the old title bar.
    delete m_toolsMenu;
    delete m_configMenu;
    delete m_viewMenu;

    // Standalone menus shown as buttons in the frameless title bar: Tools,
    // Config and View (Exit lives on the title bar's close button).
    auto *toolsMenu = new QMenu(tr("&Tools"), this);
    m_toolsMenu = toolsMenu;
    toolsMenu->addAction(tr("Open &Terminal Here"), this, &MainWindow::openTerminalHere);
    toolsMenu->addAction(tr("Calculate &Checksums..."), this, &MainWindow::calculateChecksums);
    toolsMenu->addAction(tr("Com&bine Files..."), this, &MainWindow::combineFiles);
    toolsMenu->addAction(tr("S&plit File..."), this, &MainWindow::splitFile);
    toolsMenu->addAction(tr("Compar&e Files..."), this, &MainWindow::compareSelectedFiles);
    toolsMenu->addAction(tr("&Compress Selected..."), this, &MainWindow::compressSelected);
    toolsMenu->addAction(tr("&Wipe Files (secure erase)..."), this,
                         &MainWindow::secureWipeSelected);

    // Config menu: shortcuts, server connection, the delete-confirmation
    // preference, plus the commands that previously lived only in the Commands
    // menu and have no shortcut of their own (so they stay reachable).
    auto *configMenu = new QMenu(tr("Con&fig"), this);
    m_configMenu = configMenu;
    configMenu->addAction(tr("Configure &Keyboard Shortcuts..."), this,
                          &MainWindow::openShortcutsDialog);
    configMenu->addAction(tr("Connect to &Server..."), this,
                          [this] { openServerConnectDialog(false); });
    {
        // Skip the "Delete N items?" prompt for trash deletes when checked.
        // (Permanent Shift+Del always confirms regardless -- see deleteSelected.)
        QAction *noConfirm = configMenu->addAction(tr("&Delete to Trash Without Confirmation"));
        noConfirm->setCheckable(true);
        noConfirm->setChecked(!m_settings.confirmDelete());
        connect(noConfirm, &QAction::toggled, this,
                [this](bool on) { m_settings.setConfirmDelete(!on); });
    }
    {
        // Browse archives (zip/7z/tar/...) in place as folders, or treat them as
        // plain files. Applies to both panels immediately.
        QAction *archiveFolder = configMenu->addAction(tr("Open &Archives as Folders"));
        archiveFolder->setCheckable(true);
        archiveFolder->setChecked(m_settings.archiveAsFolder());
        connect(archiveFolder, &QAction::toggled, this, [this](bool on) {
            m_settings.setArchiveAsFolder(on);
            m_leftPanel->setArchiveAsFolder(on);
            m_rightPanel->setArchiveAsFolder(on);
        });
    }
    // Synchronize/Compare Directories now live in the "✳" shortcut menu; the
    // Network Neighborhood entry was removed (SMB/FTP/etc. use Connect to
    // Server instead of a gvfs browse).

    auto *viewMenu = new QMenu(tr("&View"), this);
    m_viewMenu = viewMenu;
    viewMenu->addAction(tr("&Refresh"), this, &MainWindow::refreshActivePanel);
    viewMenu->addSeparator();
    QMenu *themeMenu = viewMenu->addMenu(tr("&Theme"));
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
    };
    for (const auto &entry : themeEntries) {
        QAction *action = themeMenu->addAction(entry.label);
        action->setCheckable(true);
        action->setChecked(m_settings.theme() == entry.theme);
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, theme = entry.theme]() { setTheme(theme); });
    }

    QMenu *languageMenu = viewMenu->addMenu(tr("&Language"));
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

    viewMenu->addSeparator();

    // File-list font size: caption + "[−] <number> [+]". The number field is
    // edited directly in place; the − / + buttons step it. Range 8..18.
    {
        auto *fontWidget = new QWidget(viewMenu);
        auto *fontLayout = new QHBoxLayout(fontWidget);
        fontLayout->setContentsMargins(20, 2, 12, 2);
        fontLayout->setSpacing(4);

        auto *caption = new QLabel(tr("Font size:"), fontWidget);

        auto *minusBtn = new QToolButton(fontWidget);
        minusBtn->setText(QStringLiteral("−"));
        minusBtn->setAutoRaise(true);
        minusBtn->setFocusPolicy(Qt::NoFocus);

        auto *sizeEdit = new QLineEdit(fontWidget);
        sizeEdit->setValidator(new QIntValidator(8, 18, sizeEdit));
        sizeEdit->setAlignment(Qt::AlignCenter);
        sizeEdit->setFixedWidth(36);
        sizeEdit->setText(QString::number(m_settings.listFontSize()));
        sizeEdit->setToolTip(tr("Type a size, or use − / + (8-18)"));

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
            pt = qBound(8, pt, 18);
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

        auto *fontAction = new QWidgetAction(viewMenu);
        fontAction->setDefaultWidget(fontWidget);
        viewMenu->addAction(fontAction);
    }

    viewMenu->addSeparator();
    // Show / hide the command line and the function-key bar. Both default
    // visible; use isHidden() (false until explicitly hidden) so the checkbox is
    // right on first build (before show()) and preserved across a rebuild.
    QAction *showCmdBar = viewMenu->addAction(tr("Command &Line"));
    showCmdBar->setCheckable(true);
    showCmdBar->setChecked(!m_commandBar->isHidden());
    connect(showCmdBar, &QAction::toggled, this, [this](bool on) {
        m_commandBar->setVisible(on);
        m_settings.setShowCommandBar(on);
    });
    QAction *showFnBar = viewMenu->addAction(tr("Function &Key Bar"));
    showFnBar->setCheckable(true);
    showFnBar->setChecked(!m_functionKeyBar->isHidden());
    connect(showFnBar, &QAction::toggled, this, [this](bool on) {
        m_functionKeyBar->setVisible(on);
        m_settings.setShowFunctionKeyBar(on);
    });

    // (Folder tree is per-panel now, toggled by the button in each panel's
    // address row — no global View-menu entry. Office document preview is an
    // always-on integrated feature now, so it has no toggle here.)

    viewMenu->addSeparator();
    viewMenu->addAction(tr("Check for &Updates..."), this, &MainWindow::checkForUpdatesNow);
    viewMenu->addAction(tr("&About this Program..."), this, &MainWindow::showAboutDialog);

    // Embed the menus in our self-drawn title bar (app icon + menu buttons +
    // window buttons), placed where the menu bar would normally sit.
    m_titleBar = new TitleBar(this, {toolsMenu, configMenu, viewMenu});
    m_titleBar->setCursor(Qt::ArrowCursor); // don't inherit the window resize cursor
    setMenuWidget(m_titleBar);
    // Clicking the title-bar "New Version" badge opens the pending-update dialog.
    connect(m_titleBar, &TitleBar::updateRequested, this, &MainWindow::showUpdateDialog);

    setupFeatureBatch();
}

// External-device hot-plug watcher, SMB neighbourhood browser, and the daily
// background update check. Kept out of the (already large) constructor body.
void MainWindow::setupFeatureBatch() {
    // Removable-device hot-plug: when a new USB stick / phone / drive appears and
    // the preference is on, mount it and open it in a fresh, activated tab.
    m_deviceMonitor = new RemovableDeviceMonitor(this);

    // The folder trees organise themselves around devices and live connections.
    // Both panels share one registry so each can see (and grey out) the other's
    // connections; hot-plug and connect/disconnect drive the rebuilds, no polling.
    m_connRegistry = new NetworkTreeRegistry(this);
    m_leftPanel->setTreeSources(m_deviceMonitor, m_connRegistry);
    m_rightPanel->setTreeSources(m_deviceMonitor, m_connRegistry);

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

    // SMB neighbourhood discovery is owned here and injected into the
    // external-connection picker. Kick off one background scan at startup and
    // cache the results (4h): opening the picker then shows hosts instantly from
    // cache, only rescanning once the cache goes stale.
    m_smbBrowser = new SmbHostBrowser(this);
    m_smbBrowser->startDiscovery();

    // Once-a-day background update check: if we haven't checked today, ask the
    // server quietly. A found update only lights the title-bar badge (no popup);
    // the user opens it when they choose.
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    if (m_settings.updateLastCheckDate() != today) {
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
        connect(checker, &UpdateChecker::checkFailed, this,
                [checker](const QString &) { checker->deleteLater(); });
        checker->checkForUpdates();
    }

    // Trim the thumbnail disk cache back under its limit, once, a few seconds
    // in. Deferred rather than immediate because it stats every stored file and
    // startup has better things to do with the disk; see
    // ThumbnailCache::scheduleMaintenance().
    ThumbnailCache::instance().scheduleMaintenance();
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
        if (event->type() == QEvent::MouseMove) {
            if (me->buttons() == Qt::NoButton) {
                // setCursor() on the window is inherited by every child that has
                // no cursor of its own, so off the edge we must UNSET it (not
                // force Arrow) — otherwise it overrides a child's own cursor
                // (e.g. the header's column-resize cursor).
                if (edges != Qt::Edges())
                    setCursor(cursorForEdges(edges));
                else
                    unsetCursor();
            }
        } else if (edges != Qt::Edges() && me->button() == Qt::LeftButton) {
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
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
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
    // The label is refreshed every call so a language-change re-run picks up the
    // new tr(); the QShortcut itself is created only once (re-running would
    // duplicate it and double-fire).
    m_commandLabels[id] = label;
    if (m_shortcuts.contains(id)) {
        for (auto &entry : m_shortcutOrder)
            if (entry.first == id) {
                entry.second = label;
                break;
            }
        return;
    }
    m_shortcutDefaults[id] = defaultSeq;
    m_shortcutOrder.append({id, label});
    m_shortcutHandlers[id] = handler; // also invokable from the "*" menu

    auto *sc = new QShortcut(m_settings.shortcut(id, defaultSeq), this);
    sc->setContext(Qt::WindowShortcut);
    connect(sc, &QShortcut::activated, this, handler);
    m_shortcuts[id] = sc;
}

void MainWindow::registerCommand(const QString &id, const QString &label,
                                  std::function<void()> handler) {
    m_commandLabels[id] = label; // refreshed on a language-change re-run
    if (!m_shortcutHandlers.contains(id))
        m_shortcutHandlers[id] = handler;
}

void MainWindow::runFunctionKey(int index) {
    if (index < 0 || index >= 6)
        return;
    auto it = m_shortcutHandlers.constFind(m_fkeyCommands[index]);
    if (it != m_shortcutHandlers.constEnd() && it.value())
        it.value()();
}

void MainWindow::updateFunctionKeyLabels() {
    for (int i = 0; i < 6; ++i) {
        const QString label = m_commandLabels.value(m_fkeyCommands[i], m_fkeyCommands[i]);
        m_functionKeyBar->setLabel(i, QStringLiteral("F%1  %2").arg(3 + i).arg(label));
    }
}

QString MainWindow::pickCommandId(const QString &title, const QString &currentId) {
    // List every command by label so the user can pick a replacement.
    QList<QPair<QString, QString>> commands; // (label, id)
    for (auto it = m_commandLabels.constBegin(); it != m_commandLabels.constEnd(); ++it)
        commands.append({it.value(), it.key()});
    std::sort(commands.begin(), commands.end(),
              [](const auto &a, const auto &b) { return a.first.localeAwareCompare(b.first) < 0; });

    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(420, 480);

    // Two columns: function name (left) and its shortcut (right-aligned).
    auto *tree = new QTreeWidget(&dlg);
    tree->setColumnCount(2);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    tree->setIndentation(0);
    tree->setUniformRowHeights(true);
    // DTK's default hover fill is a dark gray that clashes with the dark row
    // text in the light theme. Override hover/selection with palette-derived
    // colors so the text stays legible in both themes.
    {
        const QColor hl = tree->palette().color(QPalette::Highlight);
        const QColor hlText = tree->palette().color(QPalette::HighlightedText);
        const QColor txt = tree->palette().color(QPalette::Text);
        tree->setStyleSheet(
            QStringLiteral("QTreeView::item:hover:!selected { background: rgba(%1,%2,%3,45); "
                           "color: %4; }"
                           "QTreeView::item:selected { background: %5; color: %6; }")
                .arg(hl.red())
                .arg(hl.green())
                .arg(hl.blue())
                .arg(txt.name())
                .arg(hl.name())
                .arg(hlText.name()));
    }
    QTreeWidgetItem *currentItem = nullptr;
    for (const auto &c : commands) {
        const QString &id = c.second;
        QKeySequence key = m_shortcuts.value(id) ? m_shortcuts.value(id)->key()
                                                  : m_shortcutDefaults.value(id);
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
    auto it = m_shortcutHandlers.constFind(cmd);
    if (it != m_shortcutHandlers.constEnd() && it.value())
        it.value()();
}

void MainWindow::updateExtraKeyButtons() {
    m_leadingCommand = m_settings.extraKeyCommand("leading", "external-connect");
    m_trailingCommand = m_settings.extraKeyCommand("trailing", "notepad");
    m_functionKeyBar->setLeadingIcon(QIcon(QStringLiteral(":/icons/ext-connect.svg")));
    m_functionKeyBar->setTrailingIcon(QIcon(QStringLiteral(":/icons/notepad.svg")));
    m_functionKeyBar->setLeadingToolTip(m_commandLabels.value(m_leadingCommand, m_leadingCommand));
    m_functionKeyBar->setTrailingToolTip(
        m_commandLabels.value(m_trailingCommand, m_trailingCommand));
}

namespace {
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
    const auto protocol = static_cast<GvfsMounter::Protocol>(c.protocol);
    const QString password = c.anonymous ? QString() : ConnectionStore::loadPassword(c.id);
    switch (protocol) {
    case GvfsMounter::Protocol::Sftp: {
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
    case GvfsMounter::Protocol::Ftp: {
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
    case GvfsMounter::Protocol::WebDav:
    case GvfsMounter::Protocol::WebDavs: {
        auto p = std::make_shared<CurlWebDavProvider>();
        const bool useHttps = protocol == GvfsMounter::Protocol::WebDavs;
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
    case GvfsMounter::Protocol::Smb: {
        auto p = std::make_shared<SmbProvider>();
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
} // namespace

void MainWindow::openExternalConnections() {
    if (!m_activePanel)
        return;
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
            [this](const SavedConnection &conn) {
                auto native = providerForSaved(conn);
                if (!native.provider) {
                    ttc::critical(this, tr("Connection Failed"),
                                  tr("Unsupported connection type."));
                    return;
                }
                // Connect asynchronously on a worker thread; the status line
                // shows "connecting / reconnecting / failed". Never blocks the UI.
                const QString path =
                    conn.remotePath.isEmpty() ? QStringLiteral("/") : conn.remotePath;
                // Open in a fresh tab on the left panel, not the active tab.
                FilePanel *panel = beginServerConnection();
                panel->model()->connectNetwork(native.provider, native.connectFn, path);
                // Show the host name on the tab immediately, before it connects.
                const QString label =
                    conn.user.isEmpty() ? conn.host : conn.user + QLatin1Char('@') + conn.host;
                panel->setConnectingLabel(label, native.provider->scheme());
                // Wire the credential-retry factory so a wrong/missing keyring
                // password surfaces a login prompt AND the re-entered password
                // actually reconnects -- without this, provideCredentials had no
                // factory and silently discarded what the user typed.
                if (native.authFactory)
                    panel->model()->setAuthContext(label, native.authFactory);
                // Record the connection so it is re-established (with its label) on
                // next launch, and store the remote path we're opening.
                SavedConnection persist = conn;
                persist.remotePath = path;
                panel->setActiveTabConnInfo(persist);
                panel->navigateTo(path);
            });
    connect(dlg, &ExternalConnectDialog::openSmbHost, this, [this](const QString &hostName) {
        if (hostName.isEmpty())
            return;
        // Browse the host's shares anonymously; "/" lists the shares available.
        // Connect asynchronously so an unreachable host never freezes the UI.
        auto provider = std::make_shared<SmbProvider>();
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
        smbInfo.protocol = static_cast<int>(GvfsMounter::Protocol::Smb);
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
    });
    // Manager button next to the "Saved Connections" header: open the connection
    // manager (add/edit/delete saved bookmarks; manual connect lives here too).
    connect(dlg, &ExternalConnectDialog::openConnectionManager, this,
            [this] { openServerConnectDialog(false); });

    // Pop up directly above the leading function-key button that launched it.
    dlg->popUpAbove(m_functionKeyBar->leadingButtonGlobalRect());
}

void MainWindow::toggleNotepad() {
    // A floating fly-out anchored above the trailing function-key button that
    // launched it (mirrors the external-connection panel), rather than a docked
    // third column. Non-modal; it auto-saves and deletes itself on close.
    auto *pad = new NotepadPanel(this);
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
    UpdateDialog dlg(m_pendingUpdate, this);
    connect(&dlg, &UpdateDialog::restartRequested, qApp, &QApplication::quit);
    dlg.exec();
}

void MainWindow::showShortcutMenu(FilePanel *panel, const QPoint &globalPos) {
    QMenu menu(this);

    auto addEntry = [&](const QString &label, const QString &keyText, std::function<void()> act) {
        // Description on the left, key right-aligned via the tab.
        QAction *action = menu.addAction(label + QLatin1Char('\t') + keyText);
        connect(action, &QAction::triggered, this, [act]() {
            if (act)
                act();
        });
    };

    // A curated set of useful-but-not-obvious, panel-scoped shortcuts (not the
    // full list). Each references a registered shortcut by id for its key and
    // action, but uses a clearer description here.
    struct Item {
        const char *id;
        QString label;
    };
    const Item items[] = {
        {"toggleHidden", tr("Show / hide hidden files")},
        {"quickFilter", tr("Filter the current panel (type to narrow)")},
        {"quickView", tr("Quick view (preview in the other panel)")},
        {"syncOther", tr("Point the other panel at this directory")},
        {"swapPanels", tr("Swap the two panels")},
        {"calcSize", tr("Calculate folder size")},
        {"undo", tr("Undo the last rename / move")},
        {"multiRename", tr("Multi-rename tool")},
        {"search", tr("Search files")},
    };
    for (const Item &item : items) {
        const QString id = QString::fromLatin1(item.id);
        auto handler = m_shortcutHandlers.constFind(id);
        if (handler == m_shortcutHandlers.constEnd())
            continue;
        const QKeySequence seq =
            m_shortcuts.value(id) ? m_shortcuts.value(id)->key() : m_shortcutDefaults.value(id);
        addEntry(item.label, seq.toString(QKeySequence::NativeText), handler.value());
    }

    // Select/unselect by wildcard mask -- these live on the panel (+/-), not in
    // the registered shortcut table, so wire them explicitly.
    addEntry(tr("Select files by pattern (e.g. *.zip)"), QStringLiteral("+"), [panel]() {
        if (panel)
            panel->selectByPattern(true);
    });
    addEntry(tr("Unselect files by pattern"), QStringLiteral("-"), [panel]() {
        if (panel)
            panel->selectByPattern(false);
    });
    addEntry(tr("Invert selection"), QStringLiteral("*"), [panel]() {
        if (panel)
            panel->invertSelection();
    });
    if (panel) {
        const QString label = panel->isThumbnailMode() ? tr("Switch to list view")
                                                        : tr("Switch to thumbnail view");
        // Read the key back from the registered shortcut so a user rebind via
        // Config > Keyboard Shortcuts is reflected here too.
        const QKeySequence seq = m_shortcuts.value(QStringLiteral("toggleViewMode"))
                                     ? m_shortcuts.value(QStringLiteral("toggleViewMode"))->key()
                                     : m_shortcutDefaults.value(QStringLiteral("toggleViewMode"));
        addEntry(label, seq.toString(QKeySequence::NativeText), [panel]() {
            panel->toggleViewMode();
        });
    }

    // Directory tools relocated here from the Config menu.
    menu.addSeparator();
    addEntry(tr("Synchronize directories..."), QString(), [this]() { openSyncDialog(); });
    addEntry(tr("Compare directories (by time)"), QString(),
             [this]() { compareDirectories(); });

    // Align the menu's right edge with the active panel's right edge -- for the
    // left panel that's the splitter between the two panels -- instead of
    // letting it open rightward from the button and spill past the divider.
    QPoint pos = globalPos;
    if (panel) {
        const int menuWidth = menu.sizeHint().width();
        const int rightEdge = panel->mapToGlobal(QPoint(panel->width(), 0)).x();
        pos.setX(rightEdge - menuWidth);
    }
    menu.exec(pos);
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

    // F3-F8 are reassignable slots: the key and the bottom-bar button both run
    // whichever command the slot points at.
    const char *fkeyDefaults[6] = {"view", "edit", "copy", "move", "mkdir", "delete"};
    for (int i = 0; i < 6; ++i) {
        // Record each command's default F-key so the change dialog can show it.
        m_shortcutDefaults[QString::fromLatin1(fkeyDefaults[i])] =
            QKeySequence(static_cast<int>(Qt::Key_F3) + i);
        m_fkeyCommands[i] =
            m_settings.functionKeyCommand(i, QString::fromLatin1(fkeyDefaults[i]));
        if (!m_shortcutsBuilt) {
            auto *sc = new QShortcut(QKeySequence(static_cast<int>(Qt::Key_F3) + i), this);
            sc->setContext(Qt::WindowShortcut);
            connect(sc, &QShortcut::activated, this, [this, i] { runFunctionKey(i); });
        }
    }
    if (!m_shortcutsBuilt) {
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
        m_shortcutsBuilt = true;
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
    const auto answer = ttc::warning(
        this, tr("Secure Wipe"),
        tr("Securely erase %1 item(s) (%2 bytes)?\n\n"
           "Their contents will be overwritten on disk and then deleted. This is "
           "IRREVERSIBLE: the files do NOT go to the trash and cannot be recovered.")
            .arg(paths.size())
            .arg(total),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    // Non-modal: overwriting runs on a background thread inside the dialog.
    auto *dlg = new SecureWipeDialog(paths, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &SecureWipeDialog::filesChanged, this, [this] {
        m_leftPanel->refresh();
        m_rightPanel->refresh();
    });
    dlg->show();
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

void MainWindow::swapPanels() {
    // While the preview is up it occupies one splitter slot and the panel it
    // displaced is parked off-screen, so exchanging the two panels' paths would
    // shuffle a pane the user cannot see -- from the user's side nothing moves
    // except the file list under the preview. What "swap sides" means here is
    // to move the preview to the other side: the visible panel and the preview
    // trade places.
    if (m_quickViewActive) {
        const QList<int> sizes = m_panelSplitter->sizes();
        FilePanel *visible = otherPanel(m_quickViewPanel);
        const int visibleIndex = m_panelSplitter->indexOf(visible);

        // Park the visible panel where the preview sat, then put the preview in
        // the slot just vacated. Taking the preview out first would leave the
        // splitter momentarily holding `visible` twice.
        m_panelSplitter->replaceWidget(m_quickViewIndex, m_quickViewPanel);
        m_quickViewPanel->show();
        m_panelSplitter->replaceWidget(visibleIndex, m_quickView);
        m_quickView->show();

        m_quickViewPanel = visible;
        m_quickViewIndex = visibleIndex;

        // The panel that just came back is now the only one on screen, so it
        // takes focus -- the preview follows the active panel's selection, and
        // leaving focus on the hidden panel would make the preview track a
        // cursor nobody can see.
        FilePanel *revealed = otherPanel(visible);
        setActivePanel(revealed);
        revealed->view()->setFocus();

        // Sides swapped, so the ratio has to swap with them or a narrow preview
        // would stay narrow while changing sides.
        QList<int> swapped = sizes;
        std::reverse(swapped.begin(), swapped.end());
        m_panelSplitter->setSizes(swapped);
        updateQuickView();
        return;
    }

    // Exchange the backends, not the path strings: a remote path resolved by the
    // other panel's backend would land it somewhere else (every backend here is
    // POSIX-rooted, so "/home" resolves on a share and on this machine alike).
    // Moving the connections themselves makes the swap mean what it says, and
    // costs nothing extra when both sides are local.
    m_leftPanel->exchangeLocationWith(m_rightPanel);
}

void MainWindow::openTerminalHere() {
    if (!m_activePanel)
        return;
    const QString cwd = m_activePanel->currentPath();
    static const QStringList terminals = {QStringLiteral("deepin-terminal"),
                                          QStringLiteral("x-terminal-emulator"),
                                          QStringLiteral("gnome-terminal"),
                                          QStringLiteral("konsole"),
                                          QStringLiteral("xfce4-terminal"),
                                          QStringLiteral("xterm")};
    for (const QString &term : terminals) {
        if (!QStandardPaths::findExecutable(term).isEmpty()) {
            QProcess::startDetached(term, {}, cwd);
            return;
        }
    }
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
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty())
        return;
    bool ok = false;
    const QString app = ttc::getText(this, tr("Open With"),
                                              tr("Application command:"), QLineEdit::Normal,
                                              QString(), &ok);
    if (!ok || app.isEmpty())
        return;
    FileProvider *prov = m_activePanel->model()->provider();
    if (prov && !prov->displayName().isEmpty()) {
        // Network tab: the command needs a real file, not the provider's path.
        fetchRemoteCopy(m_activePanel, path,
                        [app](const QString &localPath) {
                            QProcess::startDetached(app, {localPath});
                        });
        return;
    }
    QProcess::startDetached(app, {path});
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
        if (rec.provider)
            m_queue->enqueueProviderRename(rec.provider, rec.fromPath, rec.toName);
        else
            m_queue->enqueueRename(rec.fromPath, rec.toName);
        break;
    case UndoRecord::Move:
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

    if (m_quickViewActive) {
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
        return;
    }
    if (!m_activePanel)
        return;
    // Replace the *inactive* panel with the preview.
    m_quickViewPanel = otherPanel(m_activePanel);
    m_quickViewIndex = m_panelSplitter->indexOf(m_quickViewPanel);
    m_panelSplitter->replaceWidget(m_quickViewIndex, m_quickView);
    m_quickView->show();
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
    if (!m_previewTempDir)
        m_previewTempDir = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/FileCommander-preview-XXXXXX"));
    return (m_previewTempDir && m_previewTempDir->isValid()) ? m_previewTempDir->path()
                                                             : QString();
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
        // Local / archive-entry: currentPreviewPath() already yields a real path.
        m_quickView->showFile(m_activePanel->currentPreviewPath());
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
    if (QuickView::canStreamPreview(entry) && entry != m_streamFailedEntry) {
        const QString url =
            MpvStreamSource::publish(m_activePanel->model()->providerPtr(), entry);
        if (!url.isEmpty()) {
            m_quickView->showFile(url);
            return;
        }
    }

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
    if (!m_openTempDir) {
        m_openTempDir =
            new QTemporaryDir(QDir::tempPath() + QStringLiteral("/FileCommander-open-XXXXXX"));
        // Deliberately kept past shutdown: an application we launched may still
        // be reading (or about to read) a copy, and pulling the file out from
        // under a document the user is looking at is worse than leaving bytes in
        // /tmp, which the system clears anyway. This is also why the dir is never
        // deleted while the window lives -- we cannot know when a launched
        // application is done with its file.
        m_openTempDir->setAutoRemove(false);
    }
    return (m_openTempDir && m_openTempDir->isValid()) ? m_openTempDir->path() : QString();
}

QString MainWindow::ensureArchiveTempDir() {
    if (!m_archiveTempDir)
        m_archiveTempDir =
            new QTemporaryDir(QDir::tempPath() + QStringLiteral("/FileCommander-archive-XXXXXX"));
    // Unlike the open-with copies above, these are NOT kept past shutdown. Nothing
    // outside this process ever sees them (the archive is browsed in-app), each
    // one is already deleted when its browse ends, and an archive is exactly the
    // kind of file that can be several gigabytes -- so the auto-remove default
    // stands as the backstop for a copy whose browse a crash cut short.
    return (m_archiveTempDir && m_archiveTempDir->isValid()) ? m_archiveTempDir->path() : QString();
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
        return GvfsMounter::localPathFor(provider.get(), path);
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

void MainWindow::splitFile() {
    if (!m_activePanel)
        return;
    const QString source = m_activePanel->currentEntryPath();
    if (source.isEmpty() || QFileInfo(source).isDir()) {
        ttc::information(this, tr("Split File"), tr("Select a file to split."));
        return;
    }
    bool ok = false;
    const int mb = ttc::getInt(this, tr("Split File"), tr("Part size (MB):"), 100, 1,
                                         1000000, 1, &ok);
    if (!ok)
        return;
    const QString destDir = otherPanel(m_activePanel)->currentPath();
    const qint64 partSize = static_cast<qint64>(mb) * 1024 * 1024;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher]() {
        QApplication::restoreOverrideCursor();
        const QStringList parts = watcher->result();
        m_leftPanel->refresh();
        m_rightPanel->refresh();
        if (parts.isEmpty())
            ttc::warning(this, tr("Split File"), tr("Failed to split the file."));
        else
            ttc::information(this, tr("Split File"),
                                     tr("Created %1 part(s).").arg(parts.size()));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&FileSplitter::split, source, partSize, destDir,
                                          static_cast<QString *>(nullptr)));
}

void MainWindow::combineFiles() {
    if (!m_activePanel)
        return;
    const QString first = m_activePanel->currentEntryPath();
    if (first.isEmpty())
        return;
    const QString base = FileSplitter::baseNameForPart(first);
    if (base.isEmpty()) {
        ttc::information(this, tr("Combine Files"),
                                 tr("Select the first part (e.g. name.001) of a split file."));
        return;
    }
    const QString destPath = QDir(otherPanel(m_activePanel)->currentPath()).filePath(base);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher]() {
        QApplication::restoreOverrideCursor();
        m_leftPanel->refresh();
        m_rightPanel->refresh();
        if (!watcher->result())
            ttc::warning(this, tr("Combine Files"), tr("Failed to merge the parts."));
        else
            ttc::information(this, tr("Combine Files"), tr("Parts merged."));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&FileSplitter::merge, first, destPath,
                                          static_cast<QString *>(nullptr)));
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

    proc->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
}

void MainWindow::openShortcutsDialog() {
    QMap<QString, QKeySequence> current;
    for (auto it = m_shortcuts.constBegin(); it != m_shortcuts.constEnd(); ++it)
        current[it.key()] = it.value()->key();

    ShortcutsDialog dlg(m_shortcutOrder, current, m_shortcutDefaults, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto updated = dlg.resultShortcuts();
    for (auto it = updated.constBegin(); it != updated.constEnd(); ++it) {
        if (!m_shortcuts.contains(it.key()))
            continue;
        m_shortcuts[it.key()]->setKey(it.value());
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
    const SavedConnection c = panel->tabConnInfo(index);
    if (c.host.isEmpty())
        return;
    auto native = providerForSaved(c);
    if (!native.provider) {
        ttc::critical(this, tr("重新连接"), tr("不支持的连接类型。"));
        return;
    }
    const QString path = c.remotePath.isEmpty() ? QStringLiteral("/") : c.remotePath;
    const QString label = c.user.isEmpty() ? c.host : c.user + QLatin1Char('@') + c.host;
    panel->connectTabTo(index, native.provider, native.connectFn, path, label, c,
                        native.authFactory);
}


void MainWindow::setTheme(Settings::Theme theme) {
    m_settings.setTheme(theme);
    m_themeManager->apply(theme);
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
        errLabel->setStyleSheet(QStringLiteral("color:#e04a4a"));
        form->addRow(errLabel);
    }
    auto *userEdit = new QLineEdit(&dlg);
    auto *passEdit = new QLineEdit(&dlg);
    passEdit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("用户名："), userEdit);
    form->addRow(tr("密码："), passEdit);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
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
    ConnectDialog dlg(this);
    if (preselectSmb)
        dlg.selectProtocol(static_cast<int>(GvfsMounter::Protocol::Smb));
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
}

FilePanel *MainWindow::panelShowingDir(const QString &dir) const {
    const QString target = QDir::cleanPath(dir);
    if (QDir::cleanPath(m_leftPanel->currentPath()) == target)
        return m_leftPanel;
    if (QDir::cleanPath(m_rightPanel->currentPath()) == target)
        return m_rightPanel;
    return nullptr;
}

FileProvider *MainWindow::providerOwningPath(const QString &path) const {
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
            return pv;
    }
    return local;
}

FileProvider *MainWindow::findLiveRemoteProvider(const QString &scheme,
                                                 const QString &displayName) const {
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
            return pv;
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
    FileProvider *dstProv = providerOwningPath(destDir);
    FileProvider *srcProv = srcProvider ? srcProvider : local;
    const bool crossProvider = (srcProv != local) || (dstProv != local);

    switch (kind) {
    case FileListView::DropActionKind::Copy:
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
    FileProvider *explicitSrcProv = nullptr;

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
    FileProvider *dstProv = providerOwningPath(destDir);
    FileProvider *srcProv = explicitSrcProv ? explicitSrcProv : providerOwningPath(sources.first());
    const bool crossProvider = (srcProv != local) || (dstProv != local);

    if (isCut) {
        FilePanel *srcPanel = panelShowingDir(QFileInfo(sources.first()).absolutePath());
        if (srcPanel) {
            m_pendingMovePanel = srcPanel;
            m_pendingMovePaths = sources;
        }
        if (crossProvider) {
            m_queue->enqueueProviderMove(srcProv, sources, dstProv, destDir);
        } else {
            recordMoveUndo(sources, destDir);
            m_queue->enqueueMove(sources, destDir);
        }
        QGuiApplication::clipboard()->clear();
    } else if (crossProvider) {
        m_queue->enqueueProviderCopy(srcProv, sources, dstProv, destDir);
    } else {
        m_queue->enqueueCopy(sources, destDir);
    }
}

void MainWindow::showFileContextMenu(FilePanel *panel, const QPoint &viewPos) {
    // Operate on whichever view (list or thumbnail) is actually showing --
    // the caller resolved `viewPos` against that same view's coordinates.
    QAbstractItemView *view = panel->activeView();
    // Right-clicking a row that isn't already selected replaces the
    // selection with just that row, matching Explorer/Nautilus/TC.
    const QModelIndex idx = view->indexAt(viewPos);
    if (idx.isValid() && !view->selectionModel()->isSelected(idx))
        view->setCurrentIndex(idx);

    QMenu menu(this);
    menu.addAction(tr("Open"), this, &MainWindow::openWithDefault);
    menu.addAction(tr("Open With..."), this, &MainWindow::openWith);
    menu.addSeparator();
    menu.addAction(tr("View"), this, &MainWindow::viewCurrent);
    menu.addAction(tr("Edit"), this, &MainWindow::editCurrent);
    menu.addSeparator();
    menu.addAction(tr("Copy"), this, &MainWindow::copySelected);
    menu.addAction(tr("Move"), this, &MainWindow::moveSelected);
    menu.addAction(tr("Rename"), this, &MainWindow::renameCurrent);
    menu.addAction(tr("Delete"), this, [this]() { deleteSelected(false); });
    menu.addSeparator();
    menu.addAction(tr("Cut"), this, &MainWindow::cutSelectionToClipboard);
    menu.addAction(tr("Copy"), this, &MainWindow::copySelectionToClipboard);
    menu.addSeparator();
    menu.addAction(tr("Compress Selected..."), this, &MainWindow::compressSelected);
    // Smart (Bandizip-style) extraction, offered only for a single archive row.
    const QString cursorPath = panel->currentEntryPath();
    if (!cursorPath.isEmpty() && ArchiveHandler::isSupportedArchive(cursorPath)) {
        menu.addAction(tr("Extract Here"), this, &MainWindow::extractArchiveHere);
        menu.addAction(tr("Extract To..."), this, &MainWindow::extractArchiveToDir);
    }
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [panel]() {
        const QStringList paths = panel->selectedPaths();
        if (!paths.isEmpty())
            QGuiApplication::clipboard()->setText(paths.join('\n'));
    });
    menu.addSeparator();
    menu.addAction(tr("Calculate Folder Size"), this, &MainWindow::calculateSizes);
    menu.addAction(tr("Properties..."), this, &MainWindow::showProperties);
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

void MainWindow::smartExtractArchive(const QString &archivePath, const QString &destDir) {
    QString source = archivePath;
    QString base = destDir;
    QString finalDir;
    for (;;) {
        QString err;
        const ArchiveHandler::SmartResult res = ArchiveHandler::smartExtract(source, base, &err);
        if (!res.ok) {
            ttc::warning(this, tr("Extract"), tr("Extraction failed: %1").arg(err));
            return;
        }
        finalDir = res.finalDir;
        if (res.nestedArchivePath.isEmpty())
            break;
        const auto answer = ttc::question(
            this, tr("Nested archive"),
            tr("The result contains a single archive:\n%1\n\nExtract it too?")
                .arg(QFileInfo(res.nestedArchivePath).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            break;
        source = res.nestedArchivePath;
        base = QFileInfo(res.nestedArchivePath).absolutePath();
    }

    // Refresh whichever panel is showing the destination so the new files appear.
    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        if (panel && panel->currentPath() == destDir)
            panel->refresh();
    }
    ttc::information(this, tr("Extract"), tr("Extracted archive to %1").arg(finalDir));
}

void MainWindow::extractArchiveHere() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || !ArchiveHandler::isSupportedArchive(path))
        return;
    smartExtractArchive(path, m_activePanel->currentPath());
}

void MainWindow::extractArchiveToDir() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || !ArchiveHandler::isSupportedArchive(path))
        return;
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
    FileProvider *srcProv = m_activePanel->model()->provider();
    FileProvider *dstProv = dest->model()->provider();
    LocalFileProvider *local = LocalFileProvider::instance();
    if (srcProv != local || dstProv != local) {
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
        m_queue->enqueueCopyAs(sources.first(), target);
        return;
    }
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

    FileProvider *srcProv = m_activePanel->model()->provider();
    FileProvider *dstProv = dest->model()->provider();
    LocalFileProvider *local = LocalFileProvider::instance();
    if (srcProv != local || dstProv != local) {
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
        FileProvider *prov = m_activePanel->model()->provider();
        if (prov != LocalFileProvider::instance())
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
    FileProvider *prov = m_activePanel->model()->provider();
    const bool remote = prov != LocalFileProvider::instance();
    const bool goesPermanent = permanent || remote;

    // Trash deletes can skip the prompt when the user turned confirmation off
    // (Config menu). Permanent deletes (Shift+Del, or any remote delete) always
    // confirm -- they can't be undone from the trash.
    if (goesPermanent || m_settings.confirmDelete()) {
        const qint64 total = sumSizes(m_activePanel->model(), paths);
        const auto answer = ttc::question(
            this, tr("Confirm Delete"),
            tr("Delete %1 item(s) (%2 bytes)?%3")
                .arg(paths.size())
                .arg(total)
                .arg(goesPermanent ? tr("\nThis is permanent and will NOT go to the trash.")
                                   : QString()));
        if (answer != QMessageBox::Yes)
            return;
    }
    // Remember what we're deleting (and where) so the queue-finished handler
    // removes exactly these rows and selects the next file without a full rescan.
    m_pendingDeletePanel = m_activePanel;
    m_pendingDeletePaths = paths;
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
    if (!ArchiveHandler::create(dlg.archivePath(), sources, dlg.format(), &err)) {
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
    if (m_activePanel)
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
    delete m_archiveTempDir;
    m_archiveTempDir = nullptr;

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
            t.path = snap.at(i).first;
            t.selectedFiles = snap.at(i).second;
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
