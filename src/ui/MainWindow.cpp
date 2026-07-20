#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QShortcut>
#include <QTimer>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "ArchiveBrowserDialog.h"
#include "ArchiveHandler.h"
#include "CommandBar.h"
#include "CompressDialog.h"
#include "FilePanel.h"
#include "FileListView.h"
#include "FileSystemModel.h"
#include "FunctionKeyBar.h"
#include "ImageViewer.h"
#include "OperationQueue.h"
#include "SearchDialog.h"
#include "SessionManager.h"
#include "Settings.h"
#include "ThemeManager.h"
#include "TextEditor.h"
#include "TextViewer.h"
#include "dialogs/CompareDialog.h"
#include "dialogs/MultiRenameDialog.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/OverwriteConfirmDialog.h"
#include "dialogs/PropertiesDialog.h"
#include "dialogs/ShortcutsDialog.h"
#include "dialogs/SyncDialog.h"

namespace {
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
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("Total Commander for Linux"));
    resize(1200, 700);

    const QByteArray savedGeometry = m_settings.windowGeometry();
    if (!savedGeometry.isEmpty())
        restoreGeometry(savedGeometry);

    m_themeManager = new ThemeManager(this);
    m_themeManager->apply(m_settings.theme());

    auto *splitter = new QSplitter(this);
    m_leftPanel = new FilePanel(splitter);
    m_rightPanel = new FilePanel(splitter);
    splitter->addWidget(m_leftPanel);
    splitter->addWidget(m_rightPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    // A visible divider that runs the full panel height -- tabs, breadcrumb,
    // list and status bar -- clearly separating the two panels. A solid mid
    // grey reads on both light and dark themes.
    splitter->setHandleWidth(2);
    splitter->setStyleSheet(
        QStringLiteral("QSplitter::handle { background-color: #8a8a8a; }"));

    m_folderTreeModel = new QFileSystemModel(this);
    m_folderTreeModel->setRootPath(QDir::rootPath());
    m_folderTreeModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    m_folderTree = new QTreeView(this);
    m_folderTree->setModel(m_folderTreeModel);
    m_folderTree->setRootIndex(m_folderTreeModel->index(QDir::rootPath()));
    for (int col = 1; col < m_folderTreeModel->columnCount(); ++col)
        m_folderTree->hideColumn(col);
    m_folderTree->setHeaderHidden(true);
    m_folderTree->hide(); // hidden by default; toggled via View menu
    connect(m_folderTree, &QTreeView::activated, this, [this](const QModelIndex &idx) {
        if (m_activePanel)
            m_activePanel->navigateTo(m_folderTreeModel->filePath(idx));
    });

    m_outerSplitter = new QSplitter(this);
    m_outerSplitter->addWidget(m_folderTree);
    m_outerSplitter->addWidget(splitter);
    m_outerSplitter->setStretchFactor(0, 0);
    m_outerSplitter->setStretchFactor(1, 1);

    m_functionKeyBar = new FunctionKeyBar(this);
    m_commandBar = new CommandBar(this);
    connect(m_commandBar, &CommandBar::commandSubmitted, this, &MainWindow::runCommand);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->addWidget(m_outerSplitter, 1);
    layout->addWidget(m_commandBar);
    layout->addWidget(m_functionKeyBar);
    setCentralWidget(central);
    // No global status bar: each FilePanel carries its own status strip, so
    // the function-key bar stays the bottom-most widget.

    m_queue = new OperationQueue(this);
    m_queue->setConflictHandler([this](const QString &src, const QString &dst) {
        return OverwriteConfirmDialog::ask(this, src, dst);
    });
    m_progressDialog = new OperationProgressDialog(this);
    connect(m_queue, &OperationQueue::started, this, [this](const QString &desc) {
        m_operationErrors.clear();
        m_progressDialog->setDescription(desc);
        m_progressDialog->show();
    });
    connect(m_queue, &OperationQueue::progress, m_progressDialog,
            &OperationProgressDialog::setProgress);
    connect(m_progressDialog, &OperationProgressDialog::cancelRequested, this, [this]() {
        // Actually stop the worker (and drop queued jobs), not just hide the
        // dialog. The finished handler hides it once the job unwinds.
        m_queue->cancelCurrent();
    });
    connect(m_queue, &OperationQueue::finished, this, [this](bool) {
        m_progressDialog->hide();
        m_leftPanel->refresh();
        m_rightPanel->refresh();
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
            FilePanel *other = otherPanel(panel);
            other->view()->setFocus();
            setActivePanel(other);
        });
        connect(panel, &FilePanel::pathChanged, this, [this, panel](const QString &path) {
            if (panel == m_activePanel) {
                m_commandBar->setDirectory(path);
                if (m_folderTree->isVisible()) {
                    const QModelIndex idx = m_folderTreeModel->index(path);
                    m_folderTree->setCurrentIndex(idx);
                    m_folderTree->scrollTo(idx);
                }
            }
        });
        connect(panel->view(), &FileListView::filesDropped, this, &MainWindow::handleFilesDropped);
        connect(panel->model(), &FileSystemModel::renameFailed, this, [this](const QString &msg) {
            QMessageBox::warning(this, tr("Rename"), msg);
        });

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
            if (ArchiveHandler::isSupportedArchive(path)) {
                auto *dlg = new ArchiveBrowserDialog(path, otherPanel(panel)->currentPath(), this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                dlg->show();
                return;
            }
            if (ImageViewer::isImage(path)) {
                auto *viewer = new ImageViewer();
                if (viewer->loadImage(path))
                    viewer->show();
                else
                    delete viewer;
                return;
            }
            auto *viewer = new TextViewer();
            if (viewer->loadFile(path)) {
                viewer->resize(800, 600);
                viewer->show();
            } else {
                delete viewer;
                QMessageBox::warning(this, tr("View"), tr("Could not open %1").arg(path));
            }
        });
    }

    setActivePanel(m_leftPanel);
    m_leftPanel->view()->setFocus();
    setTabOrder(m_leftPanel->view(), m_rightPanel->view());
    setTabOrder(m_rightPanel->view(), m_leftPanel->view());

    setupMenuAndToolbar();
    setupShortcuts();
}

void MainWindow::setupMenuAndToolbar() {
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("E&xit"), this, &QWidget::close);

    QMenu *commandsMenu = menuBar()->addMenu(tr("&Commands"));
    commandsMenu->addAction(tr("&Refresh"), this, &MainWindow::refreshActivePanel);
    commandsMenu->addAction(tr("&Compress Selected..."), this, &MainWindow::compressSelected);
    commandsMenu->addAction(tr("&Search Files..."), this, &MainWindow::openSearch);
    commandsMenu->addAction(tr("&Multi-Rename Tool..."), this,
                             &MainWindow::openMultiRenameDialog);
    commandsMenu->addAction(tr("S&ynchronize Directories..."), this,
                             &MainWindow::openSyncDialog);
    commandsMenu->addAction(tr("Compar&e by Content..."), this,
                             &MainWindow::compareSelectedFiles);
    commandsMenu->addAction(tr("Calculate &Occupied Space"), this,
                             &MainWindow::calculateSizes);
    commandsMenu->addSeparator();
    commandsMenu->addAction(tr("&Select by Pattern..."), this, [this] {
        if (m_activePanel)
            m_activePanel->selectByPattern(true);
    });
    commandsMenu->addAction(tr("&Unselect by Pattern..."), this, [this] {
        if (m_activePanel)
            m_activePanel->selectByPattern(false);
    });
    commandsMenu->addSeparator();
    commandsMenu->addAction(tr("Same Directory in &Other Panel"), this,
                             &MainWindow::syncOtherPanelToActive);
    commandsMenu->addAction(tr("S&wap Panels"), this, &MainWindow::swapPanels);
    commandsMenu->addSeparator();
    commandsMenu->addAction(tr("&Directory Hotlist..."), this,
                             &MainWindow::openDirectoryHotlist);
    commandsMenu->addSeparator();
    commandsMenu->addAction(tr("&Keyboard Shortcuts..."), this,
                             &MainWindow::openShortcutsDialog);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
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
    struct LanguageEntry {
        QString code;
        QString label;
    };
    const LanguageEntry languageEntries[] = {
        {"auto", tr("Auto")},
        {"en", tr("English")},
        {"zh_CN", tr("Chinese (Simplified)")},
    };
    for (const auto &entry : languageEntries) {
        QAction *action = languageMenu->addAction(entry.label);
        action->setCheckable(true);
        action->setChecked(m_settings.language() == entry.code);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, code = entry.code]() { setLanguage(code); });
    }

    viewMenu->addSeparator();
    QAction *folderTreeAction = viewMenu->addAction(tr("&Folder Tree"), this,
                                                      &MainWindow::toggleFolderTree);
    folderTreeAction->setCheckable(true);
    folderTreeAction->setChecked(false);

    auto *toolbar = addToolBar(tr("Navigation"));
    toolbar->setFocusPolicy(Qt::NoFocus);
    QAction *backAction = toolbar->addAction(tr("←"), this, &MainWindow::navigateBack);
    QAction *fwdAction = toolbar->addAction(tr("→"), this, &MainWindow::navigateForward);
    QAction *upAction = toolbar->addAction(tr("↑"), this, &MainWindow::navigateUp);
    QAction *refreshBtn = toolbar->addAction(tr("↻"), this, &MainWindow::refreshActivePanel);
    for (QAction *a : {backAction, fwdAction, upAction, refreshBtn})
        Q_UNUSED(a);
}

void MainWindow::bindShortcut(const QString &id, const QString &label,
                               const QKeySequence &defaultSeq, std::function<void()> handler) {
    m_shortcutDefaults[id] = defaultSeq;
    m_shortcutOrder.append({id, label});

    auto *sc = new QShortcut(m_settings.shortcut(id, defaultSeq), this);
    sc->setContext(Qt::WindowShortcut);
    connect(sc, &QShortcut::activated, this, handler);
    m_shortcuts[id] = sc;
}

void MainWindow::setupShortcuts() {
    bindShortcut("view", tr("View"), QKeySequence(Qt::Key_F3), [this] { viewCurrent(); });
    bindShortcut("edit", tr("Edit"), QKeySequence(Qt::Key_F4), [this] { editCurrent(); });
    bindShortcut("copy", tr("Copy"), QKeySequence(Qt::Key_F5), [this] { copySelected(); });
    bindShortcut("move", tr("Move"), QKeySequence(Qt::Key_F6), [this] { moveSelected(); });
    bindShortcut("mkdir", tr("New Folder"), QKeySequence(Qt::Key_F7),
                 [this] { makeDirectory(); });
    bindShortcut("delete", tr("Delete (to trash)"), QKeySequence(Qt::Key_F8),
                 [this] { deleteSelected(false); });
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
                     if (m_activePanel)
                         m_activePanel->toggleHiddenFiles();
                 });
    bindShortcut("calcSize", tr("Calculate Folder Size"),
                 QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Return), [this] { calculateSizes(); });
    bindShortcut("swapPanels", tr("Swap Panels"), QKeySequence(Qt::CTRL | Qt::Key_U),
                 [this] { swapPanels(); });
    bindShortcut("syncOther", tr("Same Directory in Other Panel"),
                 QKeySequence(Qt::CTRL | Qt::Key_Right), [this] { syncOtherPanelToActive(); });
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

void MainWindow::runCommand(const QString &command, const QString &directory) {
    // Run through a shell so pipes, globbing, and redirection work as typed.
    const QString cwd = directory.isEmpty() && m_activePanel ? m_activePanel->currentPath()
                                                             : directory;
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"),
                                  {QStringLiteral("-c"), command}, cwd)) {
        QMessageBox::warning(this, tr("Command"),
                             tr("Failed to run: %1").arg(command));
        return;
    }
    // A detached process finishes asynchronously; give quick commands (mkdir,
    // touch, rm) a moment to land, then refresh so their effect shows up.
    QTimer::singleShot(300, this, [this]() {
        m_leftPanel->refresh();
        m_rightPanel->refresh();
    });
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

void MainWindow::openDirectoryHotlist() {
    if (!m_activePanel)
        return;
    FilePanel *panel = m_activePanel;
    const QString currentPath = panel->currentPath();
    const QStringList favorites = m_settings.favoriteDirectories();

    QMenu menu(this);
    if (favorites.isEmpty()) {
        QAction *placeholder = menu.addAction(tr("(No favorites yet)"));
        placeholder->setEnabled(false);
    } else {
        for (const QString &favPath : favorites) {
            menu.addAction(favPath, this, [panel, favPath]() { panel->navigateTo(favPath); });
        }
    }
    menu.addSeparator();

    if (favorites.contains(currentPath)) {
        menu.addAction(tr("Remove Current Directory"), this, [this, currentPath]() {
            m_settings.removeFavoriteDirectory(currentPath);
        });
    } else {
        menu.addAction(tr("Add Current Directory"), this, [this, currentPath]() {
            m_settings.addFavoriteDirectory(currentPath);
        });
    }

    menu.exec(QCursor::pos());
}

void MainWindow::toggleFolderTree() {
    const bool nowVisible = !m_folderTree->isVisible();
    m_folderTree->setVisible(nowVisible);
    if (nowVisible && m_activePanel) {
        const QModelIndex idx = m_folderTreeModel->index(m_activePanel->currentPath());
        m_folderTree->setCurrentIndex(idx);
        m_folderTree->scrollTo(idx);
    }
}

void MainWindow::setTheme(Settings::Theme theme) {
    m_settings.setTheme(theme);
    m_themeManager->apply(theme);
}

void MainWindow::setLanguage(const QString &language) {
    m_settings.setLanguage(language);
    QMessageBox::information(this, tr("Language"),
                              tr("Restart Total Commander for the language change to take "
                                 "effect."));
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
        m_queue->enqueueMove(sources, destDir);
        QGuiApplication::clipboard()->clear();
    } else {
        m_queue->enqueueCopy(sources, destDir);
    }
}

void MainWindow::showFileContextMenu(FilePanel *panel, const QPoint &viewPos) {
    // Right-clicking a row that isn't already selected replaces the
    // selection with just that row, matching Explorer/Nautilus/TC.
    const QModelIndex idx = panel->view()->indexAt(viewPos);
    if (idx.isValid() && !panel->view()->selectionModel()->isSelected(idx))
        panel->view()->setCurrentIndex(idx);

    QMenu menu(this);
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
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [panel]() {
        const QStringList paths = panel->selectedPaths();
        if (!paths.isEmpty())
            QGuiApplication::clipboard()->setText(paths.join('\n'));
    });
    menu.addSeparator();
    menu.addAction(tr("Calculate Folder Size"), this, &MainWindow::calculateSizes);
    menu.addAction(tr("Properties..."), this, &MainWindow::showProperties);
    menu.exec(panel->view()->viewport()->mapToGlobal(viewPos));
}

void MainWindow::showBlankContextMenu(FilePanel *panel, const QPoint &viewPos) {
    setActivePanel(panel);
    QMenu menu(this);
    menu.addAction(tr("Paste"), this, &MainWindow::pasteFromClipboard);
    menu.addSeparator();
    menu.addAction(tr("New Folder"), this, &MainWindow::makeDirectory);
    menu.addSeparator();
    menu.addAction(tr("Refresh"), this, &MainWindow::refreshActivePanel);
    menu.exec(panel->view()->viewport()->mapToGlobal(viewPos));
}

void MainWindow::setActivePanel(FilePanel *panel) {
    m_activePanel = panel;
    if (panel)
        m_commandBar->setDirectory(panel->currentPath());
}

void MainWindow::viewCurrent() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty() || QFileInfo(path).isDir())
        return;

    if (ArchiveHandler::isSupportedArchive(path)) {
        auto *dlg =
            new ArchiveBrowserDialog(path, otherPanel(m_activePanel)->currentPath(), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        return;
    }

    if (ImageViewer::isImage(path)) {
        auto *viewer = new ImageViewer();
        if (viewer->loadImage(path))
            viewer->show();
        else
            delete viewer;
        return;
    }

    auto *viewer = new TextViewer();
    if (viewer->loadFile(path)) {
        viewer->resize(800, 600);
        viewer->show();
    } else {
        delete viewer;
    }
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

void MainWindow::copySelected() {
    if (!m_activePanel)
        return;
    const QStringList sources = m_activePanel->selectedPaths();
    if (sources.isEmpty())
        return;
    m_queue->enqueueCopy(sources, otherPanel(m_activePanel)->currentPath());
}

void MainWindow::moveSelected() {
    if (!m_activePanel)
        return;
    const QStringList sources = m_activePanel->selectedPaths();
    if (sources.isEmpty())
        return;
    m_queue->enqueueMove(sources, otherPanel(m_activePanel)->currentPath());
}

void MainWindow::makeDirectory() {
    if (!m_activePanel)
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
    const QStringList paths = m_activePanel->selectedPaths();
    if (paths.isEmpty())
        return;

    const qint64 total = sumSizes(paths);
    const auto answer = QMessageBox::question(
        this, tr("Confirm Delete"),
        tr("Delete %1 item(s) (%2 bytes)?%3")
            .arg(paths.size())
            .arg(total)
            .arg(permanent ? tr("\nThis is permanent and will NOT go to the trash.") : QString()));
    if (answer != QMessageBox::Yes)
        return;
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
    dlg->show();
}

void MainWindow::renameCurrent() {
    if (!m_activePanel)
        return;
    // In-place cell editing (like TC/Explorer/Nautilus) rather than a
    // modal dialog: FileSystemModel::flags()/setData() do the actual
    // rename synchronously when editing commits. NoEditTriggers stays set
    // on the view globally, so only this explicit edit() call (never a
    // stray click) can start it.
    FileListView *view = m_activePanel->view();
    const QModelIndex idx = view->currentIndex();
    if (idx.isValid())
        view->edit(idx.siblingAtColumn(FileSystemModel::NameColumn));
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
