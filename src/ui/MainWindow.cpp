#include "MainWindow.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "ArchiveBrowserDialog.h"
#include "ArchiveHandler.h"
#include "FilePanel.h"
#include "FileListView.h"
#include "FunctionKeyBar.h"
#include "ImageViewer.h"
#include "OperationQueue.h"
#include "StatusBarWidget.h"
#include "TextEditor.h"
#include "TextViewer.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/OverwriteConfirmDialog.h"

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
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("Total Commander for Linux"));
    resize(1200, 700);

    auto *splitter = new QSplitter(this);
    m_leftPanel = new FilePanel(splitter);
    m_rightPanel = new FilePanel(splitter);
    splitter->addWidget(m_leftPanel);
    splitter->addWidget(m_rightPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    m_functionKeyBar = new FunctionKeyBar(this);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_functionKeyBar);
    setCentralWidget(central);

    m_statusBarWidget = new StatusBarWidget(this);
    statusBar()->addWidget(m_statusBarWidget, 1);

    m_queue = new OperationQueue(this);
    m_queue->setConflictHandler([this](const QString &src, const QString &dst) {
        return OverwriteConfirmDialog::ask(this, src, dst);
    });
    m_progressDialog = new OperationProgressDialog(this);
    connect(m_queue, &OperationQueue::started, this, [this](const QString &desc) {
        m_progressDialog->setDescription(desc);
        m_progressDialog->show();
    });
    connect(m_queue, &OperationQueue::progress, m_progressDialog,
            &OperationProgressDialog::setProgress);
    connect(m_progressDialog, &OperationProgressDialog::cancelRequested, this, [this]() {
        m_progressDialog->hide();
    });
    connect(m_queue, &OperationQueue::finished, this, [this](bool) {
        m_progressDialog->hide();
        m_leftPanel->refresh();
        m_rightPanel->refresh();
    });
    connect(m_queue, &OperationQueue::errorOccurred, this, [this](const QString &msg) {
        QMessageBox::warning(this, tr("Operation Error"), msg);
    });

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    m_leftPanel->navigateTo(home);
    m_rightPanel->navigateTo(home);

    for (FilePanel *panel : {m_leftPanel, m_rightPanel}) {
        connect(panel, &FilePanel::panelActivated, this, &MainWindow::setActivePanel);
        connect(panel, &FilePanel::pathChanged, this, [this](const QString &) { updateStatusBar(); });
        connect(panel->view()->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this]() { updateStatusBar(); });
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
    QAction *exitAction = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence(Qt::Key_F10));

    QMenu *commandsMenu = menuBar()->addMenu(tr("&Commands"));
    QAction *refreshAction = commandsMenu->addAction(tr("&Refresh"), this,
                                                       &MainWindow::refreshActivePanel);
    refreshAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));

    auto *toolbar = addToolBar(tr("Navigation"));
    toolbar->setFocusPolicy(Qt::NoFocus);
    QAction *backAction = toolbar->addAction(tr("←"), this, &MainWindow::navigateBack);
    QAction *fwdAction = toolbar->addAction(tr("→"), this, &MainWindow::navigateForward);
    QAction *upAction = toolbar->addAction(tr("↑"), this, &MainWindow::navigateUp);
    QAction *refreshBtn = toolbar->addAction(tr("↻"), this, &MainWindow::refreshActivePanel);
    for (QAction *a : {backAction, fwdAction, upAction, refreshBtn})
        Q_UNUSED(a);
}

void MainWindow::setupShortcuts() {
    auto bind = [this](QKeySequence seq, auto slot) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, slot);
    };

    bind(QKeySequence(Qt::Key_F3), &MainWindow::viewCurrent);
    bind(QKeySequence(Qt::Key_F4), &MainWindow::editCurrent);
    bind(QKeySequence(Qt::Key_F5), &MainWindow::copySelected);
    bind(QKeySequence(Qt::Key_F6), &MainWindow::moveSelected);
    bind(QKeySequence(Qt::Key_F7), &MainWindow::makeDirectory);
    bind(QKeySequence(Qt::Key_F8), [this]() { deleteSelected(false); });
    bind(QKeySequence(Qt::SHIFT | Qt::Key_Delete), [this]() { deleteSelected(true); });
    bind(QKeySequence(Qt::Key_Delete), [this]() { deleteSelected(false); });
    bind(QKeySequence(Qt::Key_F2), &MainWindow::renameCurrent);

    bind(QKeySequence(Qt::CTRL | Qt::Key_T), [this]() {
        if (m_activePanel)
            m_activePanel->newTab();
    });
    bind(QKeySequence(Qt::CTRL | Qt::Key_W), [this]() {
        if (m_activePanel)
            m_activePanel->closeCurrentTab();
    });
    bind(QKeySequence(Qt::CTRL | Qt::Key_Tab), [this]() {
        if (m_activePanel)
            m_activePanel->nextTab();
    });
    bind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), [this]() {
        if (m_activePanel)
            m_activePanel->prevTab();
    });
}

FilePanel *MainWindow::otherPanel(FilePanel *panel) const {
    return panel == m_leftPanel ? m_rightPanel : m_leftPanel;
}

void MainWindow::setActivePanel(FilePanel *panel) {
    m_activePanel = panel;
    updateStatusBar();
}

void MainWindow::updateStatusBar() {
    if (!m_activePanel)
        return;
    const QStringList selected = m_activePanel->view()->selectionModel()->hasSelection()
                                      ? m_activePanel->selectedPaths()
                                      : QStringList();
    m_statusBarWidget->setSelectionInfo(selected.size(), sumSizes(selected),
                                         m_activePanel->model()->rowCount());
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

void MainWindow::renameCurrent() {
    if (!m_activePanel)
        return;
    const QString path = m_activePanel->currentEntryPath();
    if (path.isEmpty())
        return;
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                                    QLineEdit::Normal, QFileInfo(path).fileName(),
                                                    &ok);
    if (ok && !newName.isEmpty())
        m_queue->enqueueRename(path, newName);
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
