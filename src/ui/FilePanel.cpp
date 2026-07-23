#include "FilePanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QStandardPaths>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>

#include "ThemedDialogs.h"
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QListView>
#include <QShortcut>
#include <QStorageInfo>
#include <QTimer>
#include <QTreeView>
#include <QFileSystemModel>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "BreadcrumbBar.h"
#include "FileListView.h"
#include "IconFileView.h"
#include "FileProvider.h"
#include "ArchiveProvider.h"
#include "StatusBarWidget.h"
#include "TabBar.h"
#include "network/NetworkSession.h"
#include "ThumbnailDelegate.h"

FilePanel::FilePanel(QWidget *parent) : QWidget(parent) {
    m_model = new FileSystemModel(this);
    m_view = new FileListView(this);
    m_view->setModel(m_model);
    m_view->installEventFilter(this);

    m_tabManager = new TabManager(this);
    m_tabBar = new TabBar(this);

    // "+" at the far right of the tab strip opens a new tab in this panel,
    // lined up directly above the address row's "✳" button.
    m_addTabButton = new QToolButton(this);
    m_addTabButton->setText(QStringLiteral("+"));
    m_addTabButton->setAutoRaise(true);
    m_addTabButton->setFocusPolicy(Qt::NoFocus);
    m_addTabButton->setToolTip(tr("New Tab"));
    connect(m_addTabButton, &QToolButton::clicked, this, [this]() {
        emit panelActivated(this); // act on this panel
        newTab();
    });

    m_addressBar = new BreadcrumbBar(this);
    m_addressBar->setFocusPolicy(Qt::ClickFocus); // keep it out of the Tab chain

    // Folder-tree toggle: first item in the tab row. Shows/hides this panel's
    // own directory tree. A monochrome BMP glyph (not the 🗀 emoji, which some
    // fonts render in a fixed colour that clashes with the other chrome icons in
    // dark mode) so it follows the palette like ← → ★ ✳.
    m_treeButton = new QToolButton(this);
    m_treeButton->setText(QStringLiteral("☰"));
    m_treeButton->setAutoRaise(true);
    m_treeButton->setCheckable(true);
    m_treeButton->setFocusPolicy(Qt::NoFocus);
    m_treeButton->setToolTip(tr("Folder tree"));
    connect(m_treeButton, &QToolButton::toggled, this, [this](bool on) {
        m_dirTree->setVisible(on);
        if (on)
            syncTreeToPath(m_model->rootPath());
    });

    // Back/Forward live inline at the head of the path row (no separate
    // toolbar) to save a full row of vertical space.
    m_backButton = new QToolButton(this);
    m_backButton->setText(QStringLiteral("←"));
    m_backButton->setAutoRaise(true);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    m_backButton->setToolTip(tr("Back"));
    connect(m_backButton, &QToolButton::clicked, this, &FilePanel::goBack);

    m_forwardButton = new QToolButton(this);
    m_forwardButton->setText(QStringLiteral("→"));
    m_forwardButton->setAutoRaise(true);
    m_forwardButton->setFocusPolicy(Qt::NoFocus);
    m_forwardButton->setToolTip(tr("Forward"));
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePanel::goForward);

    // "*" opens a menu of all keyboard shortcuts (click a row to run it).
    m_starButton = new QToolButton(this);
    m_starButton->setText(QStringLiteral("✳"));
    m_starButton->setAutoRaise(true);
    m_starButton->setFocusPolicy(Qt::NoFocus);
    m_starButton->setToolTip(tr("Commands / shortcuts"));
    connect(m_starButton, &QToolButton::clicked, this, [this]() {
        emit panelActivated(this); // act on this panel
        emit shortcutMenuRequested(
            this, m_starButton->mapToGlobal(QPoint(0, m_starButton->height())));
    });

    auto *addressRow = new QWidget(this);
    auto *addressLayout = new QHBoxLayout(addressRow);
    addressLayout->setContentsMargins(0, 0, 0, 0);
    addressLayout->setSpacing(2);
    addressLayout->addWidget(m_backButton);
    addressLayout->addWidget(m_forwardButton);
    addressLayout->addWidget(m_addressBar, 1);
    addressLayout->addWidget(m_starButton);

    m_filterBar = new QLineEdit(this);
    m_filterBar->setClearButtonEnabled(true);
    m_filterBar->setPlaceholderText(tr("Filter: type to narrow the list, Esc to clear"));
    m_filterBar->hide(); // revealed on demand via showQuickFilter()
    m_filterBar->installEventFilter(this);

    m_statusBar = new StatusBarWidget(this);

    // Tab row: [tree] at the head, then the tab strip (stretch), then the
    // trailing "+". Back/Forward and the ✳ menu live in the address row below.
    auto *tabRow = new QWidget(this);
    auto *tabRowLayout = new QHBoxLayout(tabRow);
    tabRowLayout->setContentsMargins(0, 0, 0, 0);
    tabRowLayout->setSpacing(2);
    tabRowLayout->addWidget(m_treeButton, 0, Qt::AlignVCenter);
    tabRowLayout->addWidget(m_tabBar, 1);
    tabRowLayout->addWidget(m_addTabButton, 0, Qt::AlignVCenter);

    // Per-panel folder tree, to the left of the list in a splitter. Hidden until
    // the tree button is toggled on. Navigating it drives this panel.
    m_dirTreeModel = new QFileSystemModel(this);
    m_dirTreeModel->setRootPath(QDir::rootPath());
    m_dirTreeModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    m_dirTree = new QTreeView(this);
    m_dirTree->setModel(m_dirTreeModel);
    m_dirTree->setRootIndex(m_dirTreeModel->index(QDir::rootPath()));
    for (int col = 1; col < m_dirTreeModel->columnCount(); ++col)
        m_dirTree->hideColumn(col);
    m_dirTree->setHeaderHidden(true);
    m_dirTree->setFocusPolicy(Qt::ClickFocus);
    m_dirTree->hide();
    connect(m_dirTree, &QTreeView::activated, this, [this](const QModelIndex &idx) {
        navigateTo(m_dirTreeModel->filePath(idx));
    });

    // Thumbnail/icon view: shares the model and (below) the selection model with
    // the list, so a mode switch keeps the current selection. Big icons come
    // from the model's DecorationRole (IconCache).
    m_iconView = new IconFileView(this);
    m_iconView->setModel(m_model);
    m_iconView->setModelColumn(0); // Name column carries the icon + label
    m_iconView->setViewMode(QListView::IconMode);
    m_iconView->setIconSize(QSize(64, 64));
    // Grid size is set from the delegate's cell metrics once it's installed
    // (see applyThumbnailIconSize() below), not hard-coded -- a too-short grid
    // makes the selection tile overlap adjacent rows.
    m_iconView->setResizeMode(QListView::Adjust);
    m_iconView->setMovement(QListView::Static);
    // QListView::setMovement(Static) has a side-effect: it calls
    // setDragEnabled(false) and viewport->setAcceptDrops(false), which silently
    // disables ALL drag-and-drop (a Static-layout icon view is assumed to be
    // non-draggable). We want the Static grid layout but the same cross-panel
    // DnD as the list view, so re-assert drag/drop here, after setMovement.
    m_iconView->setDragEnabled(true);
    m_iconView->setDragDropMode(QAbstractItemView::DragDrop);
    m_iconView->setDefaultDropAction(Qt::CopyAction);
    m_iconView->viewport()->setAcceptDrops(true);
    m_iconView->setUniformItemSizes(true);
    m_iconView->setWordWrap(true);
    m_iconView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // No inline rename on double-click: like the list view, double-click / Enter
    // must activate (enter dir / open file). Rename stays on F2.
    m_iconView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_iconView->setSelectionModel(m_view->selectionModel()); // shared: mode switch keeps selection
    m_iconView->installEventFilter(this);
    // Mouse events are delivered to the viewport, not the view widget, so the
    // click-to-rename handling below must filter the viewport.
    m_iconView->viewport()->installEventFilter(this);
    connect(m_iconView, &QAbstractItemView::activated, this, &FilePanel::onActivated);
    // Real image/video thumbnails (with a generic-icon fallback), generated + disk
    // cached off-thread. The delegate's icon/text sizes track the View-menu font.
    m_thumbnailDelegate = new ThumbnailDelegate(m_iconView);
    m_thumbnailDelegate->setView(m_iconView);
    m_thumbnailDelegate->setIconSize(64);
    m_thumbnailDelegate->setFontPointSize(m_iconView->font().pointSize());
    m_iconView->setItemDelegate(m_thumbnailDelegate);
    // Now that the delegate is installed, size the grid from its cell metrics
    // (single source of truth) so the selection tile frames exactly one cell.
    applyThumbnailIconSize(64);

    // Single-click-on-already-selected-thumbnail starts an inline rename after
    // the double-click interval, mirroring FileListView's mouse handling (see
    // eventFilter() below); a double-click cancels it and opens instead.
    m_iconRenameClickTimer = new QTimer(this);
    m_iconRenameClickTimer->setSingleShot(true);
    connect(m_iconRenameClickTimer, &QTimer::timeout, this, [this]() {
        const QModelIndex idx(m_iconRenameClickIndex);
        m_iconRenameClickIndex = QModelIndex();
        // Only if it's still the sole selection (nothing changed while we waited).
        if (idx.isValid() && m_iconView->selectionModel()
            && m_iconView->selectionModel()->selectedIndexes().size() == 1
            && m_iconView->selectionModel()->isSelected(idx))
            m_iconView->edit(idx);
    });

    m_bodyStack = new QStackedWidget(this);
    m_bodyStack->addWidget(m_view);     // index 0: list
    m_bodyStack->addWidget(m_iconView); // index 1: thumbnails

    m_bodySplitter = new QSplitter(Qt::Horizontal, this);
    m_bodySplitter->addWidget(m_dirTree);
    m_bodySplitter->addWidget(m_bodyStack);
    m_bodySplitter->setStretchFactor(0, 0);
    m_bodySplitter->setStretchFactor(1, 1);
    m_bodySplitter->setSizes({200, 600});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(tabRow);
    layout->addWidget(addressRow);
    layout->addWidget(m_filterBar);
    layout->addWidget(m_bodySplitter, 1);
    layout->addWidget(m_statusBar);

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateStatus(); });

    connect(m_statusBar, &StatusBarWidget::zoomOutRequested, this, &FilePanel::zoomViewOut);
    connect(m_statusBar, &StatusBarWidget::zoomInRequested, this, &FilePanel::zoomViewIn);

    // Keep the chrome (tabs, breadcrumb, column header) as short as one file
    // row so the panel reads as a single dense list rather than stacked bars.
    const int rowH = qMax(m_view->fontMetrics().height(), 16) + 6;
    m_view->verticalHeader()->setDefaultSectionSize(rowH);
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->horizontalHeader()->setFixedHeight(rowH);
    // The tab bar keeps its natural height so the style vertically centres the
    // tab text; forcing it shorter left the text stuck at the bottom.
    m_addressBar->setFixedHeight(rowH);
    m_treeButton->setFixedSize(rowH, rowH);
    m_backButton->setFixedSize(rowH, rowH);
    m_forwardButton->setFixedSize(rowH, rowH);
    m_starButton->setFixedSize(rowH, rowH);
    m_addTabButton->setFixedWidth(rowH); // width matches "✳"; height follows the tab strip
    updateNavButtons();

    connect(m_filterBar, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_model->setNameFilter(text); });

    connect(m_view, &QAbstractItemView::activated, this, &FilePanel::onActivated);
    connect(m_addressBar, &BreadcrumbBar::pathActivated, this, &FilePanel::onAddressBarEntered);
    connect(m_model, &FileSystemModel::loadFinished, this, [this](int) {
        // In flat (search-results) mode there is no single directory: skip the
        // breadcrumb/tree/disk-usage sync, which all key off rootPath().
        const bool flat = m_model->isFlatMode();
        m_addressBar->setPath(flat ? QString() : m_model->rootPath());
        if (!flat)
            syncTreeToPath(m_model->rootPath());
        if (!m_pendingSelection.isEmpty()) {
            QItemSelectionModel *sel = m_view->selectionModel();
            QModelIndex first;
            for (int row = 0; row < m_model->rowCount(); ++row) {
                if (m_pendingSelection.contains(m_model->fileInfoAt(row).path())) {
                    const QModelIndex idx = m_model->index(row, 0);
                    sel->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                    if (!first.isValid())
                        first = idx;
                }
            }
            // Keep focus on (and scroll to) the first restored row -- e.g. the
            // file just renamed -- rather than letting the reload jump to the
            // top. NoUpdate leaves the selection above intact.
            if (first.isValid()) {
                sel->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
                activeView()->scrollTo(first, QAbstractItemView::EnsureVisible);
            }
            m_pendingSelection.clear();
        }
        if (m_view->model()->rowCount() > 0 && !m_view->currentIndex().isValid())
            m_view->setCurrentIndex(m_view->model()->index(0, 0));
        updateStatus();
        if (!flat) {
            const QStorageInfo storage(m_model->rootPath());
            m_statusBar->setDiskInfo(storage.bytesAvailable(), storage.bytesTotal());
        }
    });

    // Network connection status -> centred status-line message; the "Retry" link
    // in the failed state asks the model's session to reconnect.
    connect(m_model, &FileSystemModel::networkStateChanged, this,
            &FilePanel::onNetworkStateChanged);
    connect(m_statusBar, &StatusBarWidget::retryRequested, this,
            [this] { m_model->retryNetwork(); });

    connect(m_tabBar, &QTabBar::currentChanged, this, &FilePanel::onTabBarCurrentChanged);
    connect(m_tabBar, &TabBar::closeTabRequested, this, &FilePanel::closeTabAt);
    // Right-click on the tab strip opens the directory-favorites menu (owned by
    // MainWindow, which has the settings-backed favorites list).
    connect(m_tabBar, &TabBar::favoritesMenuRequested, this,
            [this](const QPoint &pos, int tabIndex) {
                emit panelActivated(this);
                emit favoritesMenuRequested(pos, tabIndex);
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

    // TC-style: gray +/- select/unselect by wildcard mask (Ctrl+A / Ctrl+Shift+A
    // still select/deselect everything).
    auto *selectPlusShortcut = new QShortcut(QKeySequence(Qt::Key_Plus), this);
    selectPlusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectPlusShortcut, &QShortcut::activated, this, [this] { selectByPattern(true); });

    auto *selectMinusShortcut = new QShortcut(QKeySequence(Qt::Key_Minus), this);
    selectMinusShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectMinusShortcut, &QShortcut::activated, this, [this] { selectByPattern(false); });
}

bool FilePanel::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == m_view || watched == m_iconView) && event->type() == QEvent::FocusIn)
        emit panelActivated(this);

    // Plain Tab (not Ctrl+Tab, which cycles tabs) jumps to the other panel.
    if (watched == m_view && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) &&
            !(ke->modifiers() & Qt::ControlModifier)) {
            emit switchPanelRequested();
            return true;
        }
    }

    // Single-click-on-already-selected-thumbnail rename, mirroring
    // FileListView's mousePress/Release/DoubleClick handling: record the index
    // on press (only if it was already the sole selection), start the rename
    // timer on release over the same cell, and cancel on a double-click (which
    // opens instead via QAbstractItemView::activated).
    if (watched == m_iconView->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        m_iconRenameClickTimer->stop();
        m_iconRenameClickIndex = QModelIndex();
        if (me->button() == Qt::LeftButton &&
            !(me->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
            const QModelIndex idx = m_iconView->indexAt(me->pos());
            // Note: QListView (icon mode) selects single items (column 0), not
            // whole rows, so selectedRows()/isRowSelected() are always empty
            // here -- use isSelected()/selectedIndexes() instead.
            if (idx.isValid() && !m_model->isParentEntry(idx.row()) &&
                (m_model->flags(idx) & Qt::ItemIsEditable) &&
                m_iconView->selectionModel()->isSelected(idx) &&
                m_iconView->selectionModel()->selectedIndexes().size() == 1)
                m_iconRenameClickIndex = idx;
        }
    } else if (watched == m_iconView->viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_iconRenameClickIndex.isValid() &&
            m_iconView->indexAt(me->pos()) == QModelIndex(m_iconRenameClickIndex))
            m_iconRenameClickTimer->start(QApplication::doubleClickInterval() + 10);
        else
            m_iconRenameClickIndex = QModelIndex();
    } else if (watched == m_iconView->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        m_iconRenameClickTimer->stop();
        m_iconRenameClickIndex = QModelIndex();
    }

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

FilePanel::NavEntry FilePanel::currentLocation() const {
    // Archive locations ("/" virtual roots) can't be restored by history, so they
    // are never recorded -- leaving an archive already drops to its host dir.
    if (m_archiveProvider)
        return {};
    NavEntry e;
    if (m_model->isFlatMode()) {
        e.flat = true;
        e.flatPaths = m_flatPaths;
    } else {
        e.dir = m_model->rootPath();
    }
    return e;
}

void FilePanel::applyHistoryEntry(const NavEntry &entry) {
    // Restoring a history entry: never re-push (goBack/goForward manage the stacks).
    if (m_archiveProvider) {
        m_archiveProvider.reset();
        m_archiveName.clear();
        m_model->setProvider(nullptr);
    }
    if (m_filterBar->isVisible()) {
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    if (entry.flat) {
        m_flatPaths = entry.flatPaths;
        m_model->setFlatEntries(entry.flatPaths);
    } else {
        m_flatPaths.clear();
        m_model->setRootPath(entry.dir);
        emit pathChanged(entry.dir);
        if (auto tab = m_tabManager->activeTab()) {
            tab->path = entry.dir;
            tab->flatPaths.clear(); // no longer a search-results listing
            tab->title.clear();     // drop the keyword title -> path-derived label
            updateActiveTabLabel();
        }
    }
}

void FilePanel::navigateTo(const QString &path) {
    // Go through the model's provider so this works for remote backends too;
    // for the local provider these are the same QDir/QFileInfo calls as before.
    FileProvider *provider = m_model->provider();
    const QString cleaned = provider->cleanPath(path);
    // For a network tab, DON'T do a synchronous isDir() here: on a remote backend
    // that is a blocking network round-trip on the GUI thread (a freeze source),
    // and callers already know the target is a directory (onActivated checks the
    // cached FileInfo). The async listing that follows reveals a bad path. Local
    // and archive backends keep the cheap in-process isDir guard.
    if (provider->displayName().isEmpty() && !provider->isDir(cleaned))
        return;
    // Snapshot where we're leaving (dir or flat listing) BEFORE mutating the view.
    const NavEntry from = currentLocation();
    if (m_filterBar->isVisible()) {
        // setRootPath() clears the model filter; just tidy the (now stale) bar.
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    if (from.isValid())
        pushHistory(from);
    m_forwardHistory.clear();
    m_flatPaths.clear(); // leaving any flat listing for a real directory
    m_model->setRootPath(cleaned);
    emit pathChanged(cleaned);

    if (auto tab = m_tabManager->activeTab()) {
        tab->path = cleaned;
        tab->flatPaths.clear(); // no longer a search-results listing
        tab->title.clear();     // drop the keyword title -> path-derived label
        updateActiveTabLabel();
    }
    updateNavButtons();
}

void FilePanel::onNetworkStateChanged(int state, int attempt) {
    // Map the session state to the centred status-line message. Connected/Idle
    // clear it (the tab shows its normal selection/disk info again).
    switch (state) {
    case NetworkSession::Connecting:
        m_statusBar->setConnectionStatus(tr("正在等待连接…"), StatusBarWidget::ConnConnecting);
        break;
    case NetworkSession::Reconnecting:
        m_statusBar->setConnectionStatus(
            tr("断线，正在重连（%1/%2）…").arg(attempt).arg(NetworkSession::kMaxReconnects),
            StatusBarWidget::ConnReconnecting);
        break;
    case NetworkSession::Failed:
        m_statusBar->setConnectionStatus(tr("多次重连失败"), StatusBarWidget::ConnFailed);
        break;
    case NetworkSession::Connected:
    case NetworkSession::Idle:
    default:
        m_statusBar->setConnectionStatus(QString(), StatusBarWidget::ConnNone);
        break;
    }
}

void FilePanel::navigateTabTo(int tabIndex, const QString &path) {
    // Make the requested tab current first so navigateTo() (which always acts on
    // the active tab) mutates it. Setting the tab-bar index drives
    // onTabBarCurrentChanged(), which saves the outgoing tab and loads this one.
    if (tabIndex >= 0 && tabIndex < m_tabBar->count() &&
        tabIndex != m_tabBar->currentIndex())
        m_tabBar->setCurrentIndex(tabIndex);
    navigateTo(path);
}

void FilePanel::showSearchResultsInNewTab(const QString &keyword, const QStringList &paths) {
    if (paths.isEmpty())
        return;
    // The results are a temporary virtual directory: give them their own tab
    // (titled after the search keyword) so the previously-browsed directory in
    // the current tab is left untouched.
    saveCurrentTabState();
    const int idx = m_tabManager->addTab(QString());
    if (auto tab = m_tabManager->tabAt(idx)) {
        tab->flatPaths = paths;
        tab->title = keyword.isEmpty() ? tr("Search results") : keyword;
    }
    m_tabManager->setActiveIndex(idx);
    m_tabBar->blockSignals(true);
    m_tabBar->addTab(tabLabelFor(m_tabManager->tabAt(idx)));
    m_tabBar->setCurrentIndex(idx);
    m_tabBar->blockSignals(false);

    // Leave archive browsing if active: a flat listing is a plain local-path set.
    if (m_archiveProvider) {
        m_archiveProvider.reset();
        m_archiveName.clear();
        m_model->setProvider(nullptr); // back to the local provider
    }
    if (m_filterBar->isVisible()) {
        m_filterBar->blockSignals(true);
        m_filterBar->clear();
        m_filterBar->hide();
        m_filterBar->blockSignals(false);
    }
    // A fresh tab has no prior location to return to; Back/Forward start empty.
    m_backHistory.clear();
    m_forwardHistory.clear();
    m_flatPaths = paths;
    m_model->setFlatEntries(paths);
    updateNavButtons();
}

void FilePanel::pushHistory(const NavEntry &entry) {
    m_backHistory.append(entry);
}

void FilePanel::updateNavButtons() {
    m_backButton->setEnabled(!m_backHistory.isEmpty());
    m_forwardButton->setEnabled(!m_forwardHistory.isEmpty());
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
    const NavEntry cur = currentLocation();
    if (cur.isValid())
        m_forwardHistory.append(cur);
    applyHistoryEntry(m_backHistory.takeLast());
    updateNavButtons();
}

void FilePanel::goForward() {
    if (m_forwardHistory.isEmpty())
        return;
    const NavEntry cur = currentLocation();
    if (cur.isValid())
        m_backHistory.append(cur);
    applyHistoryEntry(m_forwardHistory.takeLast());
    updateNavButtons();
}

void FilePanel::refresh() {
    if (!m_model->rootPath().isEmpty())
        m_model->setRootPath(m_model->rootPath());
}

bool FilePanel::isThumbnailMode() const {
    return m_bodyStack && m_bodyStack->currentWidget() == m_iconView;
}

QAbstractItemView *FilePanel::activeView() const {
    return isThumbnailMode() ? static_cast<QAbstractItemView *>(m_iconView)
                              : static_cast<QAbstractItemView *>(m_view);
}

void FilePanel::beginRenameCurrent() {
    QAbstractItemView *view = activeView();
    const QModelIndex idx = view->currentIndex();
    if (!idx.isValid() || m_model->isParentEntry(idx.row()))
        return;
    view->edit(idx.siblingAtColumn(FileSystemModel::NameColumn));
}

void FilePanel::toggleViewMode() {
    if (!m_bodyStack)
        return;
    const bool toThumb = !isThumbnailMode();
    m_bodyStack->setCurrentWidget(toThumb ? static_cast<QWidget *>(m_iconView)
                                          : static_cast<QWidget *>(m_view));
    // Carry the keyboard cursor across so arrow keys resume where they were.
    if (const QModelIndex cur = m_view->currentIndex(); cur.isValid())
        (toThumb ? static_cast<QAbstractItemView *>(m_iconView)
                 : static_cast<QAbstractItemView *>(m_view))
            ->setCurrentIndex(cur);
    (toThumb ? static_cast<QWidget *>(m_iconView) : static_cast<QWidget *>(m_view))->setFocus();
}

void FilePanel::syncTreeToPath(const QString &path) {
    if (!m_dirTree || !m_dirTree->isVisible() || path.isEmpty())
        return;
    const QModelIndex idx = m_dirTreeModel->index(path);
    if (idx.isValid()) {
        m_dirTree->setCurrentIndex(idx);
        m_dirTree->scrollTo(idx);
    }
}

void FilePanel::activateCurrentEntry() {
    // Reuse the double-click/Enter path so directories, archives and files are
    // all handled through the active provider -- not QDesktopServices on a
    // fabricated local path, which silently no-ops for network providers.
    const QModelIndex idx = activeView()->currentIndex();
    if (idx.isValid())
        onActivated(idx);
}

void FilePanel::onActivated(const QModelIndex &index) {
    const FileInfo info = m_model->fileInfoAt(index.row());
    if (!info.isValid())
        return;

    // Exiting an archive: ".." at the archive root (its provider reports the
    // parent as a local dir it can't itself descend) switches back to the local
    // filesystem at the directory the archive lives in.
    if (info.isParentEntry() && m_archiveProvider && !m_model->provider()->isDir(info.path())) {
        const QString exitDir = info.path();
        m_archiveProvider.reset();
        m_archiveName.clear();
        m_model->setProvider(nullptr); // back to the local provider
        navigateTo(exitDir);
        return;
    }

    if (info.isDir() || info.isParentEntry()) {
        navigateTo(info.path());
        return;
    }

    // Entering an archive: only from the local filesystem (no nested-archive
    // browse yet). Read-only smart browse via ArchiveProvider. Disabled when the
    // "archive as folder" preference is off -- archives then open as plain files.
    if (m_archiveAsFolder && !m_archiveProvider && ArchiveProvider::isArchivePath(info.path())) {
        QString error;
        auto provider = std::make_shared<ArchiveProvider>(info.path(), &error);
        if (provider->isValid()) {
            // Record where we came from (a real dir or a flat search-results list)
            // BEFORE switching to the archive provider, so Back returns to it.
            // currentLocation() reports {} once m_archiveProvider is set, so the
            // navigateTo("/") below can't snapshot it -- we must push it here.
            const NavEntry from = currentLocation();
            m_archiveProvider = provider;
            m_archiveName = QFileInfo(info.path()).fileName();
            m_model->setProvider(provider);
            if (from.isValid())
                pushHistory(from);
            navigateTo(QStringLiteral("/")); // archive virtual root (won't re-push)
            return;
        }
        // Not a usable archive (or unreadable): fall through to opening it.
    }

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

bool FilePanel::currentEntryIsDir() const {
    const QModelIndex idx = m_view->currentIndex();
    return idx.isValid() && m_model->fileInfoAt(idx.row()).isDir();
}

qint64 FilePanel::currentEntrySize() const {
    const QModelIndex idx = m_view->currentIndex();
    return idx.isValid() ? m_model->fileInfoAt(idx.row()).size() : 0;
}

FileInfo FilePanel::currentEntryInfo() const {
    const QModelIndex idx = m_view->currentIndex();
    return idx.isValid() ? m_model->fileInfoAt(idx.row()) : FileInfo();
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

QString FilePanel::currentPreviewPath() {
    const QString entry = currentEntryPath();
    if (entry.isEmpty() || !isArchive())
        return entry; // local filesystem: the path is already real
    // Inside an archive: directories aren't previewable; files are extracted to
    // a temp file (name + extension preserved) so the viewers see a real path.
    if (m_model->provider()->isDir(entry))
        return {};
    auto *ap = static_cast<ArchiveProvider *>(m_archiveProvider.get());
    const QString real = ap->materialize(entry);
    prefetchArchiveNeighbors();
    return real.isEmpty() ? QString() : real;
}

void FilePanel::prefetchArchiveNeighbors() {
    if (!m_archiveProvider)
        return;
    const QModelIndex cur = m_view->currentIndex();
    if (!cur.isValid())
        return;
    const int row = cur.row();
    // Keep the provider alive for the duration of the async extraction even if
    // the user exits the archive in the meantime.
    auto prov = m_archiveProvider;
    for (int r : {row - 1, row + 1}) {
        if (r < 0 || r >= m_model->rowCount())
            continue;
        const FileInfo fi = m_model->fileInfoAt(r);
        if (!fi.isValid() || fi.isDir() || fi.isParentEntry())
            continue;
        const QString p = fi.path();
        QtConcurrent::run(
            [prov, p]() { static_cast<ArchiveProvider *>(prov.get())->materialize(p); });
    }
}

void FilePanel::setListFontSize(int pt) {
    pt = qBound(7, pt, 24);
    QFont f = m_view->font();
    f.setPointSize(pt);
    m_view->setFont(f);

    // Scale the row icons with the font so a larger font doesn't leave tiny
    // icons stranded in tall rows. The delegate honours the view's iconSize
    // (decorationSize); the row height tracks whichever of icon/text is taller,
    // unless the -/+ buttons have set an explicit override.
    const QFontMetrics fm(f);
    const int fontH = fm.height();
    const int iconPx = qBound(16, fontH + 4, 48);
    m_view->setIconSize(QSize(iconPx, iconPx));
    applyListRowHeight();

    // Thumbnail (icon) view follows the same font; its grid icon size and the
    // delegate's text point size are set when the thumbnail delegate is wired.
    if (m_iconView) {
        QFont gf = m_iconView->font();
        gf.setPointSize(pt);
        m_iconView->setFont(gf);
        applyThumbnailFontSize(pt);
    }
}

void FilePanel::applyThumbnailFontSize(int pt) {
    if (!m_iconView)
        return;
    m_lastFontPt = pt; // remembered so setThumbnailIconSize() can re-derive without a font change
    // Grow the thumbnail cell with the font: icon scales from a 64px baseline at
    // 12pt; the grid reserves room for the icon plus a two-line elided label.
    // An explicit m_thumbIconSize (from the -/+ buttons) overrides the
    // font-derived size, so zooming persists independent of the font setting.
    const int iconPx = m_thumbIconSize > 0 ? m_thumbIconSize : qBound(48, 64 * pt / 12, 160);
    applyThumbnailIconSize(iconPx);
    if (m_thumbnailDelegate)
        m_thumbnailDelegate->setFontPointSize(pt); // label text always tracks the list font
}

void FilePanel::applyThumbnailIconSize(int iconPx) {
    if (!m_iconView)
        return;
    m_iconView->setIconSize(QSize(iconPx, iconPx));
    if (m_thumbnailDelegate) {
        m_thumbnailDelegate->setIconSize(iconPx);
        // The grid height MUST equal the delegate's painted cell height, or the
        // vertical layout step is shorter than the cell and the selection tile
        // (drawn on option.rect) overlaps the rows above/below and clips the
        // label. Width adds a little inter-column breathing room. cellSizeHint()
        // is the shared source of truth with the delegate's sizeHint().
        const QSize cell = m_thumbnailDelegate->cellSizeHint(m_iconView->font());
        m_iconView->setGridSize(QSize(cell.width() + 26, cell.height()));
        m_iconView->doItemsLayout();
    }
}

void FilePanel::applyListRowHeight() {
    const QFontMetrics fm(m_view->font());
    const int autoRowH = qMax(qMax(fm.height(), m_view->iconSize().height()), 16) + 6;
    const int rowH = m_listRowHeightOverride > 0 ? m_listRowHeightOverride : autoRowH;
    m_view->verticalHeader()->setDefaultSectionSize(rowH);
    m_view->horizontalHeader()->setFixedHeight(rowH);
}

void FilePanel::setThumbnailIconSize(int px) {
    m_thumbIconSize = px > 0 ? qBound(48, px, 192) : 0;
    applyThumbnailFontSize(m_lastFontPt); // reapply: override (or auto) at the current font size
}

void FilePanel::setListRowHeight(int h) {
    m_listRowHeightOverride = h > 0 ? qBound(16, h, 64) : 0;
    applyListRowHeight();
}

void FilePanel::zoomThumbnails(int deltaPx) {
    if (!m_iconView)
        return;
    const int current = m_thumbIconSize > 0 ? m_thumbIconSize : m_iconView->iconSize().width();
    m_thumbIconSize = qBound(48, current + deltaPx, 192);
    applyThumbnailIconSize(m_thumbIconSize);
    emit viewScaleChanged();
}

void FilePanel::zoomListRows(int deltaPx) {
    const QFontMetrics fm(m_view->font());
    const int autoRowH = qMax(qMax(fm.height(), m_view->iconSize().height()), 16) + 6;
    const int current = m_listRowHeightOverride > 0 ? m_listRowHeightOverride : autoRowH;
    m_listRowHeightOverride = qBound(16, current + deltaPx, 64);
    applyListRowHeight();
    emit viewScaleChanged();
}

void FilePanel::zoomViewOut() {
    if (isThumbnailMode())
        zoomThumbnails(-16);
    else
        zoomListRows(-4);
}

void FilePanel::zoomViewIn() {
    if (isThumbnailMode())
        zoomThumbnails(16);
    else
        zoomListRows(4);
}

void FilePanel::selectAll() {
    m_view->selectAll();
}

void FilePanel::deselectAll() {
    m_view->clearSelection();
}

void FilePanel::selectPathAfterReload(const QString &path) {
    if (!path.isEmpty() && !m_pendingSelection.contains(path))
        m_pendingSelection.append(path);
}

bool FilePanel::removeDeletedAndSelectNext(const QStringList &paths) {
    if (paths.isEmpty())
        return false;
    // Flat search-results listings have no directory to rescan -- refresh() is a
    // no-op there because the model's rootPath is empty. So splice the deleted
    // rows out of the model in place; removePaths() handles a flat listing the
    // same as a directory one. Keep the panel's own m_flatPaths mirror in sync
    // too, or a Back/Forward history snapshot would resurrect the deleted rows.
    const bool flat = m_model->isFlatMode();
    const int anchor = m_model->removePaths(paths);
    if (anchor < 0)
        return false; // nothing matched (e.g. the delete failed) -> caller refreshes
    if (flat)
        for (const QString &p : paths)
            m_flatPaths.removeAll(p);

    const int rows = m_model->rowCount();
    if (rows <= 0)
        return true; // directory emptied; nothing left to select

    // The removed block's old top row now holds the "next" file. If we deleted
    // the tail of the list, fall back to the new last row.
    int target = anchor >= rows ? rows - 1 : anchor;
    // Prefer a real entry over the ".." parent row when something else remains.
    if (m_model->isParentEntry(target) && rows > 1)
        target = 1;

    QAbstractItemView *view = isThumbnailMode() ? static_cast<QAbstractItemView *>(m_iconView)
                                                : static_cast<QAbstractItemView *>(m_view);
    const QModelIndex idx = m_model->index(target, 0);
    QItemSelectionModel *sel = view->selectionModel();
    sel->clearSelection();
    sel->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    sel->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
    view->scrollTo(idx, QAbstractItemView::EnsureVisible);
    updateStatus();
    return true;
}

void FilePanel::setActive(bool active) {
    m_view->setPanelActive(active);
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

void FilePanel::selectByPattern(bool select) {
    bool ok = false;
    const QString mask = ttc::getText(
        this, select ? tr("Select by Pattern") : tr("Unselect by Pattern"),
        tr("Wildcard mask (e.g. *.txt):"), QLineEdit::Normal, QStringLiteral("*"), &ok);
    if (!ok || mask.isEmpty())
        return;

    QItemSelectionModel *sel = m_view->selectionModel();
    const auto command = (select ? QItemSelectionModel::Select : QItemSelectionModel::Deselect) |
                         QItemSelectionModel::Rows;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->isParentEntry(row))
            continue;
        const FileInfo info = m_model->fileInfoAt(row);
        if (info.isValid() && FileSystemModel::matchesFilter(info.name(), mask))
            sel->select(m_model->index(row, 0), command);
    }
}

void FilePanel::calculateDirSizes() {
    QStringList dirs;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &idx : rows) {
        if (m_model->isParentEntry(idx.row()))
            continue;
        const FileInfo info = m_model->fileInfoAt(idx.row());
        if (info.isValid() && info.isDir())
            dirs << info.path();
    }
    if (dirs.isEmpty()) {
        // Nothing selected: fall back to the directory under the cursor.
        const QModelIndex cur = m_view->currentIndex();
        if (cur.isValid() && !m_model->isParentEntry(cur.row())) {
            const FileInfo info = m_model->fileInfoAt(cur.row());
            if (info.isValid() && info.isDir())
                dirs << info.path();
        }
    }

    // Capture the provider (shared) so a remote directory can be sized via the
    // provider engine, and so it stays alive for the whole walk even if the model
    // swaps providers meanwhile. Local paths take the fast QDirIterator path.
    std::shared_ptr<FileProvider> provider = m_model->providerPtr();
    for (const QString &path : dirs) {
        auto *watcher = new QFutureWatcher<qint64>(this);
        connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, path]() {
            m_model->setComputedDirSize(path, watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(
            [provider, path]() { return FileSystemModel::directorySize(provider.get(), path); }));
    }
}

QString FilePanel::tabLabelFor(const QSharedPointer<TabState> &tab) const {
    // A search-results tab carries an explicit title (the search keyword) and no
    // single directory path, so honour it before the path-derived label.
    if (tab && !tab->title.isEmpty())
        return tab->title;
    if (!tab || tab->path.isEmpty())
        return tr("New Tab");
    // Directory-name label (unchanged local behaviour).
    QString label;
    const QString name = QFileInfo(tab->path).fileName();
    if (!name.isEmpty()) {
        label = name;
    } else if (!m_archiveName.isEmpty() &&
               tab == m_tabManager->tabAt(m_tabManager->activeIndex())) {
        // The archive virtual root is "/" (no file name): label the active tab
        // with the archive's own file name instead of a bare "/".
        label = m_archiveName;
    } else {
        label = tab->path; // real root "/"
    }
    // On a network connection, prefix the connection identity (user@host) so the
    // tab shows which host it is browsing, not just the directory. The provider
    // is per-panel (shared by all tabs) but each tab keeps its own path, so this
    // only holds for the ACTIVE tab -- mirror the archive-name guard above and
    // leave background tabs (which may sit on a local path) with the plain label.
    // Local and archive backends report an empty displayName() regardless.
    if (tab == m_tabManager->tabAt(m_tabManager->activeIndex())) {
        if (FileProvider *provider = m_model->provider()) {
            const QString connection = provider->displayName();
            if (!connection.isEmpty())
                return connection + QStringLiteral(" : ") + label;
        }
    }
    return label;
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
    if (m_model->isFlatMode()) {
        // Preserve the flat search-results listing (and its keyword title) so
        // switching away and back restores it instead of a real directory.
        tab->flatPaths = m_flatPaths;
    } else {
        tab->path = m_model->rootPath();
        tab->flatPaths.clear();
        tab->title.clear();
    }
    tab->selectedFiles = selectedPaths();
}

void FilePanel::loadTabState(int index) {
    auto tab = m_tabManager->tabAt(index);
    if (!tab)
        return;
    m_backHistory.clear();
    m_forwardHistory.clear();
    m_pendingSelection = tab->selectedFiles;
    updateNavButtons();
    if (!tab->flatPaths.isEmpty()) {
        m_flatPaths = tab->flatPaths;
        m_model->setFlatEntries(tab->flatPaths);
    } else if (!tab->path.isEmpty()) {
        m_flatPaths.clear();
        m_model->setRootPath(tab->path);
    }
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

int FilePanel::closeTabsOnMount(const QString &mountRoot) {
    if (mountRoot.isEmpty())
        return 0;

    const QString prefix = mountRoot + QLatin1Char('/');
    auto onDevice = [&](const QSharedPointer<TabState> &t) {
        // Only real-directory tabs live on a mount; flat search-result tabs are
        // virtual and path-independent.
        return t && t->flatPaths.isEmpty() &&
               (t->path == mountRoot || t->path.startsWith(prefix));
    };

    // Flush the live view into the active tab so a surviving active tab keeps
    // whatever the user last navigated to (not a stale saved path).
    saveCurrentTabState();

    int affected = 0;
    // Close from the back so indices stay valid; never drop below one tab.
    for (int i = m_tabManager->count() - 1; i >= 0; --i) {
        if (m_tabManager->count() <= 1)
            break;
        if (onDevice(m_tabManager->tabAt(i))) {
            m_tabManager->closeTab(i);
            ++affected;
        }
    }

    // If the only remaining tab is still on the dead device, send it home
    // rather than leave it stranded on an unmounted path.
    if (m_tabManager->count() == 1 && onDevice(m_tabManager->tabAt(0))) {
        const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        auto t0 = m_tabManager->tabAt(0);
        t0->path = home;
        t0->flatPaths.clear();
        t0->title.clear();
        t0->selectedFiles.clear();
        ++affected;
    }

    if (affected > 0)
        syncTabBarFromManager();
    return affected;
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
