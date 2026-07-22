#include "MainWindow.h"

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
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QWidgetAction>
#include <QTreeWidget>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
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
#include "FileSplitter.h"
#include "QuickView.h"
#include "ViewerWindow.h"
#include "FileListView.h"
#include "FileSystemModel.h"
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
#include "dialogs/TransferProgressDialog.h"
#include "dialogs/OverwriteConfirmDialog.h"
#include "dialogs/PropertiesDialog.h"
#include "dialogs/ShortcutsDialog.h"
#include "dialogs/SyncDialog.h"

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

qint64 sumSizes(const QStringList &paths) {
    qint64 total = 0;
    for (const QString &p : paths) {
        QFileInfo fi(p);
        if (fi.isFile())
            total += fi.size();
    }
    return total;
}

// Builds clipboard data with both the plain text/uri-list format (read by
// virtually everything) and the GNOME x-special/gnome-copied-files
// convention (read by Nautilus/Dolphin/PCManFM) so cut vs. copy survives
// round-tripping through those file managers, not just within ttc.
QMimeData *buildFileClipboardData(const QStringList &paths, bool cut) {
    QList<QUrl> urls;
    for (const QString &path : paths)
        urls.append(QUrl::fromLocalFile(path));

    auto *mime = new QMimeData;
    mime->setUrls(urls);

    QByteArray gnomeFormat = cut ? "cut\n" : "copy\n";
    for (const QUrl &url : urls)
        gnomeFormat += url.toString().toUtf8() + "\n";
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeFormat);

    return mime;
}

// Frameless-window chrome metrics (see paintEvent / event / changeEvent).
constexpr int kShadowMargin = 16; // translucent margin: drop shadow + resize band
constexpr int kCornerRadius = 8;  // rounded window-corner radius
constexpr int kResizeGrab = 8;    // edge-resize grab band, straddling the content edge
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Frameless: we draw our own title bar (see setupMenuAndToolbar / TitleBar)
    // plus a rounded background and soft shadow in paintEvent. The window is
    // translucent so the shadow can fade into nothing at its edges.
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Total Commander for Linux"));
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
    m_queue->setConflictHandler([this](const QString &src, const QString &dst) {
        return OverwriteConfirmDialog::ask(this, src, dst);
    });
    m_queue->setErrorHandler([this](const QString &path, const QString &error) {
        QMessageBox box(QMessageBox::Warning, tr("Operation Error"),
                        tr("%1\n\n%2").arg(error, path), QMessageBox::NoButton, this);
        QPushButton *retry = box.addButton(tr("Retry"), QMessageBox::AcceptRole);
        QPushButton *skip = box.addButton(tr("Skip"), QMessageBox::RejectRole);
        QPushButton *skipAll = box.addButton(tr("Skip All"), QMessageBox::RejectRole);
        box.addButton(tr("Cancel"), QMessageBox::DestructiveRole);
        box.exec();
        if (box.clickedButton() == retry)
            return ErrorAction::Retry;
        if (box.clickedButton() == skip)
            return ErrorAction::Skip;
        if (box.clickedButton() == skipAll)
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
        if (m_pendingDeletePanel) {
            // A delete just finished: drop the vanished rows from the active
            // panel in place and move the cursor onto the next file, rather than
            // rescanning (which would reset the selection to the first row).
            FilePanel *panel = m_pendingDeletePanel;
            m_pendingDeletePanel = nullptr;
            QStringList gone;
            for (const QString &p : m_pendingDeletePaths)
                if (!QFileInfo::exists(p)) // keep any that survived a failed delete
                    gone.append(p);
            m_pendingDeletePaths.clear();
            const bool handled = panel->removeDeletedAndSelectNext(gone);
            // The other panel may show the same directory, so refresh it fully.
            FilePanel *other = (panel == m_leftPanel) ? m_rightPanel : m_leftPanel;
            other->refresh();
            if (!handled)
                panel->refresh();
        } else {
            m_leftPanel->refresh();
            m_rightPanel->refresh();
        }
        // Report all per-file failures once, not one modal per error.
        if (!m_operationErrors.isEmpty()) {
            const int shown = qMin(m_operationErrors.size(), 20);
            QString text = m_operationErrors.mid(0, shown).join(QLatin1Char('\n'));
            if (m_operationErrors.size() > shown)
                text += tr("\n... and %1 more.").arg(m_operationErrors.size() - shown);
            QMessageBox::warning(this, tr("Operation Error"), text);
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
    const QByteArray headerState = m_settings.viewHeaderState();
    if (!headerState.isEmpty()) {
        m_leftPanel->view()->horizontalHeader()->restoreState(headerState);
        m_rightPanel->view()->horizontalHeader()->restoreState(headerState);
        // A restored layout is the user's saved choice: keep it, don't auto-fit.
        m_leftPanel->view()->markColumnsManual();
        m_rightPanel->view()->markColumnsManual();
    }

    // Optional bars + splitter layout. Applied before buildTitleBarMenus() so
    // the View-menu checkmarks (which read the widgets' visibility) match.
    m_commandBar->setVisible(m_settings.showCommandBar());
    m_functionKeyBar->setVisible(m_settings.showFunctionKeyBar());
    if (const QByteArray s = m_settings.panelSplitterState(); !s.isEmpty())
        m_panelSplitter->restoreState(s);

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    SessionPanelData leftSession, rightSession;
    if (SessionManager::load(leftSession, rightSession)) {
        QVector<QPair<QString, QStringList>> leftTabs, rightTabs;
        for (const auto &t : leftSession.tabs)
            leftTabs.append({t.path, t.selectedFiles});
        for (const auto &t : rightSession.tabs)
            rightTabs.append({t.path, t.selectedFiles});
        m_leftPanel->restoreTabs(leftTabs, leftSession.activeTab);
        m_rightPanel->restoreTabs(rightTabs, rightSession.activeTab);
    } else {
        m_leftPanel->navigateTo(home);
        m_rightPanel->navigateTo(home);
    }

    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
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
        connect(panel->view()->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this]() {
                    if (m_quickViewActive)
                        m_quickViewDebounce->start(); // coalesce rapid cursor moves
                });
        connect(panel->model(), &FileSystemModel::renameFailed, this, [this](const QString &msg) {
            QMessageBox::warning(this, tr("Rename"), msg);
        });
        connect(panel->model(), &FileSystemModel::renamed, this,
                [this, panel](const QString &oldPath, const QString &newPath) {
                    m_lastUndo = UndoRecord{};
                    m_lastUndo.type = UndoRecord::Rename;
                    m_lastUndo.fromPath = newPath;
                    m_lastUndo.toName = QFileInfo(oldPath).fileName();
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
        connect(panel, &FilePanel::openRequested, this, [this](const QString &path) {
            // Double-click opens with the system's MIME-associated application.
            // (Directories and local archives are handled earlier in
            // FilePanel::onActivated and never reach here; F3 is the in-app
            // viewer.)
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                QMessageBox::warning(this, tr("Open"),
                                     tr("No application is associated with %1").arg(path));
        });
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
    configMenu->addAction(tr("Connect to &Server..."), this, [this] {
        ConnectDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted || !m_activePanel)
            return;
        if (auto provider = dlg.remoteProvider()) {
            // Native SFTP: swap the connected provider into the panel's model.
            m_activePanel->model()->setProvider(provider);
            m_activePanel->navigateTo(dlg.remotePath());
        } else if (!dlg.mountedLocalPath().isEmpty()) {
            // gvfs-mounted protocols look like a local directory.
            m_activePanel->navigateTo(dlg.mountedLocalPath());
        }
    });
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

    // Embed the menus in our self-drawn title bar (app icon + menu buttons +
    // window buttons), placed where the menu bar would normally sit.
    m_titleBar = new TitleBar(this, {toolsMenu, configMenu, viewMenu});
    m_titleBar->setCursor(Qt::ArrowCursor); // don't inherit the window resize cursor
    setMenuWidget(m_titleBar);
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

void MainWindow::changeFunctionKey(int index) {
    if (index < 0 || index >= 6)
        return;
    // List every command by label so the user can pick a replacement.
    QList<QPair<QString, QString>> commands; // (label, id)
    for (auto it = m_commandLabels.constBegin(); it != m_commandLabels.constEnd(); ++it)
        commands.append({it.value(), it.key()});
    std::sort(commands.begin(), commands.end(),
              [](const auto &a, const auto &b) { return a.first.localeAwareCompare(b.first) < 0; });

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Change F%1 Function").arg(3 + index));
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
        if (id == m_fkeyCommands[index])
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
    layout->addWidget(new QLabel(tr("Choose the function for the F%1 key:").arg(3 + index), &dlg));
    layout->addWidget(tree);
    layout->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted && tree->currentItem()) {
        m_fkeyCommands[index] = tree->currentItem()->data(0, Qt::UserRole).toString();
        m_settings.setFunctionKeyCommand(index, m_fkeyCommands[index]);
        updateFunctionKeyLabels();
    }
}

void MainWindow::showShortcutMenu(const QPoint &globalPos) {
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
    addEntry(tr("Select files by pattern (e.g. *.zip)"), QStringLiteral("+"), [this]() {
        if (m_activePanel)
            m_activePanel->selectByPattern(true);
    });
    addEntry(tr("Unselect files by pattern"), QStringLiteral("-"), [this]() {
        if (m_activePanel)
            m_activePanel->selectByPattern(false);
    });
    addEntry(tr("Invert selection"), QStringLiteral("*"), [this]() {
        if (m_activePanel)
            m_activePanel->invertSelection();
    });
    if (m_activePanel) {
        const QString label = m_activePanel->isThumbnailMode() ? tr("Switch to list view")
                                                               : tr("Switch to thumbnail view");
        addEntry(label, QString(), [this]() {
            if (m_activePanel)
                m_activePanel->toggleViewMode();
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
    if (m_activePanel) {
        const int menuWidth = menu.sizeHint().width();
        const int rightEdge = m_activePanel->mapToGlobal(QPoint(m_activePanel->width(), 0)).x();
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
        m_shortcutsBuilt = true;
    }
    updateFunctionKeyLabels();
}

void MainWindow::showProperties() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
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
    // Files only (checksums of a directory are meaningless).
    QStringList files;
    for (const QString &p : m_activePanel->selectedPaths())
        if (QFileInfo(p).isFile())
            files.append(p);
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("Checksums"),
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
    if (blockArchiveWrite(m_activePanel))
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;

    const qint64 total = sumSizes(paths);
    const auto answer = QMessageBox::warning(
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
    if (m_activePanel)
        otherPanel(m_activePanel)->navigateTo(m_activePanel->currentPath());
}

void MainWindow::swapPanels() {
    const QString left = m_leftPanel->currentPath();
    const QString right = m_rightPanel->currentPath();
    m_leftPanel->navigateTo(right);
    m_rightPanel->navigateTo(left);
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
    QMessageBox::warning(this, tr("Open Terminal"), tr("No terminal emulator found."));
}

void MainWindow::openWithDefault() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::openWith() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty())
        return;
    bool ok = false;
    const QString app = QInputDialog::getText(this, tr("Open With"),
                                              tr("Application command:"), QLineEdit::Normal,
                                              QString(), &ok);
    if (ok && !app.isEmpty())
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

void MainWindow::updateQuickView() {
    if (!m_quickViewActive || !m_activePanel)
        return;
    // An archive under the cursor previews from its raw path (a header scan);
    // any other target goes through currentPreviewPath(), which extracts an
    // archived entry to a real temp file (and prefetches neighbours) so archived
    // files preview like local ones.
    const QString entry = m_activePanel->currentEntryPath();
    if (ArchiveHandler::isSupportedArchive(entry))
        m_quickView->showFile(entry);
    else
        m_quickView->showFile(m_activePanel->currentPreviewPath());
}

void MainWindow::splitFile() {
    if (!m_activePanel)
        return;
    const QString source = m_activePanel->currentEntryPath();
    if (source.isEmpty() || QFileInfo(source).isDir()) {
        QMessageBox::information(this, tr("Split File"), tr("Select a file to split."));
        return;
    }
    bool ok = false;
    const int mb = QInputDialog::getInt(this, tr("Split File"), tr("Part size (MB):"), 100, 1,
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
            QMessageBox::warning(this, tr("Split File"), tr("Failed to split the file."));
        else
            QMessageBox::information(this, tr("Split File"),
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
        QMessageBox::information(this, tr("Combine Files"),
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
            QMessageBox::warning(this, tr("Combine Files"), tr("Failed to merge the parts."));
        else
            QMessageBox::information(this, tr("Combine Files"), tr("Parts merged."));
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
    SyncDialog dlg(m_leftPanel->currentPath(), m_rightPanel->currentPath(), this);
    dlg.exec();
    m_leftPanel->refresh();
    m_rightPanel->refresh();
}

void MainWindow::compareSelectedFiles() {
    if (!m_activePanel)
        return;

    auto onlyFiles = [](const QStringList &paths) {
        QStringList files;
        for (const QString &p : paths) {
            if (QFileInfo(p).isFile())
                files.append(p);
        }
        return files;
    };

    const QStringList activeFiles = onlyFiles(m_activePanel->selectedPaths());
    QString leftPath, rightPath;

    if (activeFiles.size() == 2) {
        leftPath = activeFiles.at(0);
        rightPath = activeFiles.at(1);
    } else {
        const QStringList otherFiles = onlyFiles(otherPanel(m_activePanel)->selectedPaths());
        if (activeFiles.size() == 1 && otherFiles.size() == 1) {
            leftPath = activeFiles.first();
            rightPath = otherFiles.first();
        }
    }

    if (leftPath.isEmpty() || rightPath.isEmpty()) {
        QMessageBox::information(
            this, tr("Compare by Content"),
            tr("Select two files to compare: either two in one panel, or one in each panel."));
        return;
    }

    auto *dlg = new CompareDialog(leftPath, rightPath, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
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
            menu->addAction(favPath, this,
                            [panel, favPath, tabIndex]() { panel->navigateTabTo(tabIndex, favPath); });
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
    populateFavoritesMenu(&menu, m_activePanel, tabIndex);
    menu.exec(globalPos);
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
    setWindowTitle(tr("Total Commander for Linux"));
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

void MainWindow::handleFilesDropped(const QStringList &sources, const QString &destDir,
                                     FileListView::DropActionKind kind) {
    switch (kind) {
    case FileListView::DropActionKind::Copy:
        m_queue->enqueueCopy(sources, destDir);
        break;
    case FileListView::DropActionKind::Move:
        recordMoveUndo(sources, destDir);
        m_queue->enqueueMove(sources, destDir);
        break;
    case FileListView::DropActionKind::Link:
        m_queue->enqueueSymlink(sources, destDir);
        break;
    }
}

void MainWindow::copySelectionToClipboard() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
    QGuiApplication::clipboard()->setMimeData(buildFileClipboardData(paths, /*cut=*/false));
}

void MainWindow::cutSelectionToClipboard() {
    if (!m_activePanel)
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;
    QGuiApplication::clipboard()->setMimeData(buildFileClipboardData(paths, /*cut=*/true));
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

    if (mime->hasFormat(QStringLiteral("x-special/gnome-copied-files"))) {
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
    if (isCut) {
        recordMoveUndo(sources, destDir);
        m_queue->enqueueMove(sources, destDir);
        QGuiApplication::clipboard()->clear();
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

void MainWindow::viewCurrent() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || QFileInfo(path).isDir())
        return;
    // F3 opens the shared preview widget in a top-level window -- same UI and
    // capabilities as the Ctrl+Q pane (image/text/video/PDF/markdown/office,
    // and a read-only archive listing). Archives are still browsed in-panel by
    // double-clicking into them, and extracted from the context menu.
    auto *w = new ViewerWindow(m_settings, path, this);
    w->show();
}

void MainWindow::smartExtractArchive(const QString &archivePath, const QString &destDir) {
    QString source = archivePath;
    QString base = destDir;
    QString finalDir;
    for (;;) {
        QString err;
        const ArchiveHandler::SmartResult res = ArchiveHandler::smartExtract(source, base, &err);
        if (!res.ok) {
            QMessageBox::warning(this, tr("Extract"), tr("Extraction failed: %1").arg(err));
            return;
        }
        finalDir = res.finalDir;
        if (res.nestedArchivePath.isEmpty())
            break;
        const auto answer = QMessageBox::question(
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
    QMessageBox::information(this, tr("Extract"), tr("Extracted archive to %1").arg(finalDir));
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
    if (path.isEmpty() || QFileInfo(path).isDir())
        return;

    if (ImageViewer::isImage(path)) {
        QMessageBox::information(this, tr("Edit"),
                                  tr("Image files can't be edited; use F3 to view."));
        return;
    }

    auto *editor = new TextEditor();
    if (editor->loadFile(path)) {
        editor->resize(900, 700);
        editor->show();
    } else {
        delete editor;
    }
}

bool MainWindow::blockArchiveWrite(FilePanel *panel) {
    if (!panel || !panel->isArchive())
        return false;
    QMessageBox::information(this, tr("Read-only"),
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
        const QString newName = QInputDialog::getText(this, tr("Copy"), tr("Copy to:"),
                                                       QLineEdit::Normal, fi.fileName(), &ok);
        if (!ok || newName.isEmpty())
            return;
        m_queue->enqueueCopyAs(sources.first(), QDir(destDir).filePath(newName));
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
        QInputDialog::getText(this, tr("New Folder"), tr("Folder name:"), QLineEdit::Normal,
                               QString(), &ok);
    if (ok && !name.isEmpty())
        m_queue->enqueueMkdir(m_activePanel->currentPath(), name);
}

void MainWindow::deleteSelected(bool permanent) {
    if (!m_activePanel)
        return;
    if (blockArchiveWrite(m_activePanel))
        return;
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;

    // Trash deletes can skip the prompt when the user turned confirmation off
    // (Config menu). Permanent deletes (Shift+Del) always confirm -- they can't
    // be undone from the trash.
    if (permanent || m_settings.confirmDelete()) {
        const qint64 total = sumSizes(paths);
        const auto answer = QMessageBox::question(
            this, tr("Confirm Delete"),
            tr("Delete %1 item(s) (%2 bytes)?%3")
                .arg(paths.size())
                .arg(total)
                .arg(permanent ? tr("\nThis is permanent and will NOT go to the trash.")
                               : QString()));
        if (answer != QMessageBox::Yes)
            return;
    }
    // Remember what we're deleting (and where) so the queue-finished handler
    // removes exactly these rows and selects the next file without a full rescan.
    m_pendingDeletePanel = m_activePanel;
    m_pendingDeletePaths = paths;
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
        QMessageBox::warning(this, tr("Compress"), tr("Compression failed: %1").arg(err));
        return;
    }
    otherPanel(m_activePanel)->refresh();
}

void MainWindow::openSearch() {
    if (!m_activePanel)
        return;
    auto *dlg = new SearchDialog(m_activePanel->currentPath(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    FilePanel *panel = m_activePanel;
    connect(dlg, &SearchDialog::navigateRequested, this, [panel](const QString &path) {
        QFileInfo info(path);
        panel->navigateTo(info.isDir() ? path : info.absolutePath());
    });
    // "Send to panel": list every result in the *currently* active panel as a
    // flat cross-directory view (Back / breadcrumb / refresh leaves it).
    connect(dlg, &SearchDialog::feedToPanelRequested, this, [this](const QStringList &paths) {
        if (m_activePanel)
            m_activePanel->showSearchResults(paths);
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
    m_settings.setWindowGeometry(saveGeometry());
    // Persist the shared column layout + sort from the left panel's header.
    m_settings.setViewHeaderState(m_leftPanel->view()->horizontalHeader()->saveState());
    // Persist the panel divider position.
    m_settings.setPanelSplitterState(m_panelSplitter->saveState());

    SessionPanelData leftSession, rightSession;
    for (const auto &t : m_leftPanel->tabSnapshot())
        leftSession.tabs.append({t.first, t.second});
    leftSession.activeTab = m_leftPanel->activeTabIndex();
    for (const auto &t : m_rightPanel->tabSnapshot())
        rightSession.tabs.append({t.first, t.second});
    rightSession.activeTab = m_rightPanel->activeTabIndex();
    SessionManager::save(leftSession, rightSession);

    QMainWindow::closeEvent(event);

    // MainWindow is the only window that should keep the app alive, but
    // other top-level widgets exist (e.g. m_progressDialog, constructed up
    // front and only shown conditionally) that can otherwise prevent Qt's
    // quitOnLastWindowClosed from firing reliably. Quit explicitly rather
    // than depend on that heuristic.
    qApp->quit();
}
