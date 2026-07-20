#include "FilePanel.h"

#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QShortcut>
#include <QVBoxLayout>

#include "BreadcrumbBar.h"
#include "FileListView.h"
#include "StatusBarWidget.h"
#include "TabBar.h"

FilePanel::FilePanel(QWidget *parent) : QWidget(parent) {
    m_model = new FileSystemModel(this);
    m_view = new FileListView(this);
    m_view->setModel(m_model);
    m_view->installEventFilter(this);

    m_tabManager = new TabManager(this);
    m_tabBar = new TabBar(this);

    m_addressBar = new BreadcrumbBar(this);
    m_addressBar->setFocusPolicy(Qt::ClickFocus); // keep it out of the Tab chain

    m_filterBar = new QLineEdit(this);
    m_filterBar->setClearButtonEnabled(true);
    m_filterBar->setPlaceholderText(tr("Filter: type to narrow the list, Esc to clear"));
    m_filterBar->hide(); // revealed on demand via showQuickFilter()
    m_filterBar->installEventFilter(this);

    m_statusBar = new StatusBarWidget(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_tabBar);
    layout->addWidget(m_addressBar);
    layout->addWidget(m_filterBar);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_statusBar);

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateStatus(); });

    connect(m_filterBar, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_model->setNameFilter(text); });

    connect(m_view, &QAbstractItemView::activated, this, &FilePanel::onActivated);
    connect(m_addressBar, &BreadcrumbBar::pathActivated, this, &FilePanel::onAddressBarEntered);
    connect(m_model, &FileSystemModel::loadFinished, this, [this](int) {
        m_addressBar->setPath(m_model->rootPath());
        if (!m_pendingSelection.isEmpty()) {
            QItemSelectionModel *sel = m_view->selectionModel();
            for (int row = 0; row < m_model->rowCount(); ++row) {
                if (m_pendingSelection.contains(m_model->fileInfoAt(row).path()))
                    sel->select(m_model->index(row, 0),
                                QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
            m_pendingSelection.clear();
        }
        if (m_view->model()->rowCount() > 0 && !m_view->currentIndex().isValid())
            m_view->setCurrentIndex(m_view->model()->index(0, 0));
        updateStatus();
    });

    connect(m_tabBar, &QTabBar::currentChanged, this, &FilePanel::onTabBarCurrentChanged);
    connect(m_tabBar, &TabBar::newTabRequested, this, &FilePanel::newTab);
    connect(m_tabBar, &TabBar::closeTabRequested, this, &FilePanel::closeTabAt);
    connect(m_tabBar, &TabBar::closeOthersRequested, this, [this](int idx) {
        m_tabManager->closeOthers(idx);
        syncTabBarFromManager();
    });
    connect(m_tabBar, &TabBar::closeToRightRequested, this, [this](int idx) {
        m_tabManager->closeToRight(idx);
        syncTabBarFromManager();
    });
    connect(m_tabBar, &TabBar::copyPathRequested, this, [this](int idx) {
        if (auto tab = m_tabManager->tabAt(idx))
            QGuiApplication::clipboard()->setText(tab->path);
    });

    const int firstTab = m_tabManager->addTab(QString());
    m_tabBar->blockSignals(true);
    m_tabBar->addTab(tr("New Tab"));
    m_tabBar->setCurrentIndex(firstTab);
    m_tabBar->blockSignals(false);
    m_tabManager->setActiveIndex(firstTab);

    auto *upShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
    upShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(upShortcut, &QShortcut::activated, this, &FilePanel::navigateUp);

    auto *backShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    backShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(backShortcut, &QShortcut::activated, this, &FilePanel::goBack);

    auto *fwdShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
    fwdShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(fwdShortcut, &QShortcut::activated, this, &FilePanel::goForward);

    auto *selectAllShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_A), this);
    selectAllShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAllShortcut, &QShortcut::activated, this, &FilePanel::selectAll);

    auto *deselectAllShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), this);
    deselectAllShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deselectAllShortcut, &QShortcut::activated, this, &FilePanel::deselectAll);

    auto *invertShortcut = new QShortcut(QKeySequence(Qt::Key_Asterisk), this);
    invertShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(invertShortcut, &QShortcut::activated, this, &FilePanel::invertSelection);

    auto *selectPlusShortcut = new QShortcut(QKeySequence(Qt::Key_Plus), this);
    selectPlusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectPlusShortcut, &QShortcut::activated, this, &FilePanel::selectAll);

    auto *selectMinusShortcut = new QShortcut(QKeySequence(Qt::Key_Minus), this);
    selectMinusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectMinusShortcut, &QShortcut::activated, this, &FilePanel::deselectAll);
}

bool FilePanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_view && event->type() == QEvent::FocusIn)
        emit panelActivated(this);

    if (watched == m_filterBar && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            hideQuickFilter();
            return true;
        }
        // Enter / Down hand control to the list without losing the filter.
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter ||
            ke->key() == Qt::Key_Down) {
            m_view->setFocus();
            if (m_model->rowCount() > 0 && !m_view->currentIndex().isValid())
                m_view->setCurrentIndex(m_model->index(0, 0));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FilePanel::showQuickFilter() {
    m_filterBar->show();
    m_filterBar->setFocus();
    m_filterBar->selectAll();
}

void FilePanel::hideQuickFilter() {
    m_filterBar->clear(); // textChanged -> setNameFilter("") restores full list
    m_filterBar->hide();
    m_view->setFocus();
}

void FilePanel::navigateTo(const QString &path) {
    const QString cleaned = QDir(path).absolutePath();
    if (!QFileInfo(cleaned).isDir())
        return;
    if (m_filterBar->isVisible()) {
        // setRootPath() clears the model filter; just tidy the (now stale) bar.
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    if (!m_model->rootPath().isEmpty())
        pushHistory(m_model->rootPath());
    m_forwardHistory.clear();
    m_model->setRootPath(cleaned);
    emit pathChanged(cleaned);

    if (auto tab = m_tabManager->activeTab()) {
        tab->path = cleaned;
        updateActiveTabLabel();
    }
}

void FilePanel::pushHistory(const QString &fromPath) {
    m_backHistory.append(fromPath);
}

void FilePanel::updateStatus() {
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    int selectedCount = 0;
    qint64 selectedBytes = 0;
    for (const QModelIndex &idx : rows) {
        if (m_model->isParentEntry(idx.row()))
            continue;
        const FileInfo info = m_model->fileInfoAt(idx.row());
        if (!info.isValid())
            continue;
        ++selectedCount;
        if (!info.isDir())
            selectedBytes += info.size();
    }
    // Total objects excludes the synthesized ".." row.
    int total = m_model->rowCount();
    if (total > 0 && m_model->isParentEntry(0))
        --total;
    m_statusBar->setSelectionInfo(selectedCount, selectedBytes, total);
}

void FilePanel::navigateUp() {
    QDir dir(m_model->rootPath());
    if (dir.cdUp())
        navigateTo(dir.absolutePath());
}

void FilePanel::goBack() {
    if (m_backHistory.isEmpty())
        return;
    m_forwardHistory.append(m_model->rootPath());
    const QString path = m_backHistory.takeLast();
    m_model->setRootPath(path);
    emit pathChanged(path);
}

void FilePanel::goForward() {
    if (m_forwardHistory.isEmpty())
        return;
    m_backHistory.append(m_model->rootPath());
    const QString path = m_forwardHistory.takeLast();
    m_model->setRootPath(path);
    emit pathChanged(path);
}

void FilePanel::refresh() {
    if (!m_model->rootPath().isEmpty())
        m_model->setRootPath(m_model->rootPath());
}

void FilePanel::onActivated(const QModelIndex &index) {
    const FileInfo info = m_model->fileInfoAt(index.row());
    if (!info.isValid())
        return;
    if (info.isDir() || info.isParentEntry())
        navigateTo(info.path());
    else
        emit openRequested(info.path());
}

void FilePanel::onAddressBarEntered(const QString &path) {
    navigateTo(path);
}

QString FilePanel::currentEntryPath() const {
    const QModelIndex idx = m_view->currentIndex();
    if (!idx.isValid())
        return {};
    return m_model->fileInfoAt(idx.row()).path();
}

QStringList FilePanel::selectedPaths() const {
    QStringList paths;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &idx : rows) {
        if (m_model->isParentEntry(idx.row()))
            continue;
        paths.append(m_model->fileInfoAt(idx.row()).path());
    }
    if (paths.isEmpty()) {
        const QString cur = currentEntryPath();
        if (!cur.isEmpty())
            paths.append(cur);
    }
    return paths;
}

void FilePanel::selectAll() {
    m_view->selectAll();
}

void FilePanel::deselectAll() {
    m_view->clearSelection();
}

void FilePanel::invertSelection() {
    QItemSelection full(m_model->index(0, 0),
                         m_model->index(m_model->rowCount() - 1, m_model->columnCount() - 1));
    m_view->selectionModel()->select(full, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

void FilePanel::toggleHiddenFiles() {
    // Re-scans the current directory; loadFinished refreshes the status line.
    m_model->setShowHiddenFiles(!m_model->showHiddenFiles());
}

QString FilePanel::tabLabelFor(const QSharedPointer<TabState> &tab) const {
    if (!tab || tab->path.isEmpty())
        return tr("New Tab");
    const QString name = QFileInfo(tab->path).fileName();
    return name.isEmpty() ? tab->path : name; // root "/" has no file name
}

void FilePanel::updateActiveTabLabel() {
    const int idx = m_tabManager->activeIndex();
    if (auto tab = m_tabManager->tabAt(idx))
        m_tabBar->setTabText(idx, tabLabelFor(tab));
}

void FilePanel::syncTabBarFromManager() {
    m_tabBar->blockSignals(true);
    while (m_tabBar->count() > 0)
        m_tabBar->removeTab(0);
    for (int i = 0; i < m_tabManager->count(); ++i)
        m_tabBar->addTab(tabLabelFor(m_tabManager->tabAt(i)));
    const int active = m_tabManager->activeIndex();
    if (active >= 0 && active < m_tabBar->count())
        m_tabBar->setCurrentIndex(active);
    m_tabBar->blockSignals(false);
    loadTabState(m_tabManager->activeIndex());
}

void FilePanel::saveCurrentTabState() {
    auto tab = m_tabManager->activeTab();
    if (!tab)
        return;
    tab->path = m_model->rootPath();
    tab->selectedFiles = selectedPaths();
}

void FilePanel::loadTabState(int index) {
    auto tab = m_tabManager->tabAt(index);
    if (!tab)
        return;
    m_backHistory.clear();
    m_forwardHistory.clear();
    m_pendingSelection = tab->selectedFiles;
    if (!tab->path.isEmpty())
        m_model->setRootPath(tab->path);
}

void FilePanel::onTabBarCurrentChanged(int index) {
    if (index < 0)
        return;
    saveCurrentTabState();
    m_tabManager->setActiveIndex(index);
    loadTabState(index);
}

void FilePanel::closeTabAt(int index) {
    if (m_tabManager->count() <= 1)
        return; // always keep at least one tab open
    m_tabManager->closeTab(index);
    syncTabBarFromManager();
}

void FilePanel::newTab() {
    saveCurrentTabState();
    const QString path = m_model->rootPath();
    const int idx = m_tabManager->addTab(path);
    m_tabBar->blockSignals(true);
    m_tabBar->addTab(tabLabelFor(m_tabManager->tabAt(idx)));
    m_tabBar->setCurrentIndex(idx);
    m_tabBar->blockSignals(false);
    m_tabManager->setActiveIndex(idx);
    loadTabState(idx);
}

void FilePanel::closeCurrentTab() {
    closeTabAt(m_tabBar->currentIndex());
}

void FilePanel::nextTab() {
    const int count = m_tabBar->count();
    if (count > 1)
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % count);
}

void FilePanel::prevTab() {
    const int count = m_tabBar->count();
    if (count > 1)
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() - 1 + count) % count);
}

QVector<QPair<QString, QStringList>> FilePanel::tabSnapshot() {
    saveCurrentTabState(); // flush the live view's path/selection into the active tab
    QVector<QPair<QString, QStringList>> result;
    for (int i = 0; i < m_tabManager->count(); ++i) {
        auto tab = m_tabManager->tabAt(i);
        result.append({tab->path, tab->selectedFiles});
    }
    return result;
}

void FilePanel::restoreTabs(const QVector<QPair<QString, QStringList>> &tabs, int activeIndex) {
    if (tabs.isEmpty())
        return;

    // Reuse the single tab created in the constructor as tab 0 instead of
    // adding a duplicate empty one.
    auto tab0 = m_tabManager->tabAt(0);
    tab0->path = tabs.at(0).first;
    tab0->selectedFiles = tabs.at(0).second;

    for (int i = 1; i < tabs.size(); ++i) {
        const int idx = m_tabManager->addTab(tabs.at(i).first);
        m_tabManager->tabAt(idx)->selectedFiles = tabs.at(i).second;
    }

    const int clamped = qBound(0, activeIndex, m_tabManager->count() - 1);
    m_tabManager->setActiveIndex(clamped);
    syncTabBarFromManager(); // rebuilds the tab bar and loads the (now correct) active tab
}
