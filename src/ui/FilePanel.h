#pragma once

#include <QPair>
#include <QPersistentModelIndex>
#include <QVector>
#include <QWidget>

#include <memory>

#include "FileSystemModel.h"
#include "TabManager.h"

class BreadcrumbBar;
class FileListView;
class StatusBarWidget;
class TabBar;
class QLineEdit;
class QToolButton;
class QTreeView;
class QFileSystemModel;
class QSplitter;
class QListView;
class QStackedWidget;
class ThumbnailDelegate;
class QAbstractItemView;
class QTimer;

// One side of the dual-pane layout: address bar + file list + per-panel
// back/forward history. Two of these live in MainWindow (left/right).
class FilePanel : public QWidget {
    Q_OBJECT

public:
    explicit FilePanel(QWidget *parent = nullptr);

    QString currentPath() const { return m_model->rootPath(); }
    void navigateTo(const QString &path);
    // Activates `tabIndex` (if valid and not already current) and navigates it to
    // `path`. Used by the tab-strip favorites menu so a chosen favorite lands in
    // the right-clicked tab. tabIndex < 0 targets the active tab.
    void navigateTabTo(int tabIndex, const QString &path);
    // Shows a flat listing of arbitrary file paths (e.g. Ctrl+F search results
    // spanning many directories) in place of the current directory. Any normal
    // navigation (Back, breadcrumb, double-click, refresh) leaves this mode.
    void showSearchResults(const QStringList &paths);
    void navigateUp();
    void goBack();
    void goForward();
    void refresh();

    // Path under the keyboard cursor (not necessarily selected) -- used by
    // F3/F4 to know which single file to open.
    QString currentEntryPath() const;
    QStringList selectedPaths() const;

    // A real, on-disk path for the current entry suitable for the preview
    // viewers. For a normal directory this is just currentEntryPath(); inside an
    // archive it extracts the entry to a temp file (and prefetches its
    // neighbours) so Ctrl+Q previews archived files like local ones. Returns ""
    // for a directory / unpreviewable entry.
    QString currentPreviewPath();

    // Sets the point size of the file-list font and rescales the row height /
    // header height to match. Point size is clamped to 7..24.
    void setListFontSize(int pt);

    void selectAll();
    void deselectAll();
    void invertSelection();
    void toggleHiddenFiles();

    // Queues a path to be selected, focused, and scrolled into view once the
    // pending directory reload finishes -- used after an inline rename so the
    // listing doesn't snap back to the top.
    void selectPathAfterReload(const QString &path);

    // Removes just-deleted paths from the listing in place (no directory
    // rescan) and moves the cursor/selection onto the row that slid into the
    // gap -- i.e. the "next" file. Returns true if it handled the removal;
    // false if nothing matched (the caller should fall back to refresh()).
    bool removeDeletedAndSelectNext(const QStringList &paths);

    // Marks this panel as the active one (of the two). Softens the inactive
    // panel's selection colour so the active panel's cursor row stands out.
    void setActive(bool active);

    // Prompts for a wildcard mask (e.g. *.txt) and adds matching files to the
    // selection (select=true) or removes them (select=false).
    void selectByPattern(bool select);

    // Recursively computes the size of each selected directory (or the one
    // under the cursor) off the UI thread and shows it in the Size column.
    void calculateDirSizes();

    void newTab();
    void closeCurrentTab();
    void nextTab();
    void prevTab();

    // Reveals the quick-filter box and gives it focus. Esc (handled in the
    // event filter) hides it and restores the full listing.
    void showQuickFilter();
    void hideQuickFilter();

    // Session persistence: plain Qt types (not TabState) so MainWindow can
    // hand these to core/config's SessionManager without ui depending on
    // it, or core depending on ui.
    QVector<QPair<QString, QStringList>> tabSnapshot();
    int activeTabIndex() const { return m_tabManager->activeIndex(); }
    void restoreTabs(const QVector<QPair<QString, QStringList>> &tabs, int activeIndex);

    FileSystemModel *model() const { return m_model; }
    FileListView *view() const { return m_view; }
    // The icon (thumbnail-mode) view, for callers (MainWindow's context menu)
    // that need to wire it up alongside view().
    QListView *iconView() const { return m_iconView; }
    // Whichever of view()/iconView() is currently visible -- context menu,
    // rename, etc. should all act on this one so they work in both modes.
    QAbstractItemView *activeView() const;

    // True while browsing inside an archive: the backend is read-only, so
    // MainWindow blocks write ops (delete/rename/mkdir/move-in/paste) here.
    bool isArchive() const { return m_archiveProvider != nullptr; }

    // List <-> thumbnail (icon) view. The * menu offers the toggle with a label
    // that depends on the current mode.
    bool isThumbnailMode() const;
    void toggleViewMode();

    // Starts an in-place rename of the active view's current index (F2 /
    // context-menu "Rename"), matching the list view's behaviour in thumbnail
    // mode too.
    void beginRenameCurrent();

    // Explicit per-panel thumbnail icon size / list row height, driven by the
    // status-bar -/+ buttons and persisted by MainWindow (per side, per mode).
    // 0 means "auto": derive the size from the View-menu font instead.
    void setThumbnailIconSize(int px);
    int thumbnailIconSize() const { return m_thumbIconSize; }
    void setListRowHeight(int h);
    int listRowHeight() const { return m_listRowHeightOverride; }

    // When false, archives open as plain files instead of being browsed in place
    // as folders. Set from the "archive as folder" config preference.
    void setArchiveAsFolder(bool on) { m_archiveAsFolder = on; }

    // Re-applies translated text (the status-bar object/selection counts) after a
    // live UI-language change.
    void retranslate() { updateStatus(); }

signals:
    void pathChanged(const QString &path);
    void panelActivated(FilePanel *panel);
    // A file (not a directory) was double-clicked / Enter-pressed.
    void openRequested(const QString &path);
    // Tab pressed in the list: caller should activate the other panel.
    void switchPanelRequested();
    // The "*" button was clicked: caller should pop a shortcut menu at pos.
    void shortcutMenuRequested(const QPoint &globalPos);
    // Tab strip right-clicked: caller should pop the directory-favorites menu.
    // tabIndex is the right-clicked tab (-1 if the click missed the tabs).
    void favoritesMenuRequested(const QPoint &globalPos, int tabIndex);
    // The status-bar -/+ buttons changed this panel's thumbnail size or list
    // row height: caller (MainWindow) should persist the new value.
    void viewScaleChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onActivated(const QModelIndex &index);
    void onAddressBarEntered(const QString &path);
    void onTabBarCurrentChanged(int index);

private:
    // A single back/forward history location: either a real directory, or a flat
    // "virtual directory" of arbitrary paths (Ctrl+F search results fed to the
    // panel). Modelling flat listings as history entries lets Back/Forward return
    // to a search result set within the session, not just to real directories.
    struct NavEntry {
        bool flat = false;      // true => flatPaths is the listing; false => dir
        QString dir;            // directory path (when !flat)
        QStringList flatPaths;  // flat listing entries (when flat)
        bool isValid() const { return flat ? !flatPaths.isEmpty() : !dir.isEmpty(); }
    };
    // Snapshots what the view currently shows, for pushing onto a history stack.
    NavEntry currentLocation() const;
    // Restores a history entry into the view (dir scan or flat listing).
    void applyHistoryEntry(const NavEntry &entry);
    void pushHistory(const NavEntry &entry);
    void updateStatus();
    void updateNavButtons();
    QString tabLabelFor(const QSharedPointer<TabState> &tab) const;
    void syncTabBarFromManager();
    void closeTabAt(int index);
    // Selects + scrolls the folder tree to `path` when the tree is visible.
    void syncTreeToPath(const QString &path);
    // Scales the thumbnail-view cell (icon + label) to the given font size so the
    // grid grows/shrinks with the View-menu font setting (unless overridden by
    // an explicit m_thumbIconSize from the -/+ buttons).
    void applyThumbnailFontSize(int pt);
    // Applies an explicit icon pixel size to the thumbnail grid/delegate,
    // independent of font size. Shared by applyThumbnailFontSize() and the -/+
    // zoom handlers.
    void applyThumbnailIconSize(int iconPx);
    // Recomputes and applies the list view's row height from either
    // m_listRowHeightOverride or the current font/icon metrics.
    void applyListRowHeight();
    // Status-bar -/+ handlers: adjust whichever mode is currently showing.
    void zoomViewOut();
    void zoomViewIn();
    void zoomThumbnails(int deltaPx);
    void zoomListRows(int deltaPx);
    // Extracts the archive entries immediately before/after the current one in
    // the background, so stepping through archived files previews instantly.
    void prefetchArchiveNeighbors();
    void saveCurrentTabState();
    void loadTabState(int index);
    void updateActiveTabLabel();

    BreadcrumbBar *m_addressBar;
    QToolButton *m_treeButton; // "🗀" toggles this panel's folder tree; first in the row
    QToolButton *m_backButton;
    QToolButton *m_forwardButton;
    QToolButton *m_starButton;
    QTreeView *m_dirTree = nullptr;            // per-panel folder tree (hidden by default)
    QFileSystemModel *m_dirTreeModel = nullptr;
    QSplitter *m_bodySplitter = nullptr;       // [tree | body]
    QListView *m_iconView = nullptr;           // thumbnail/icon mode (shares m_model)
    ThumbnailDelegate *m_thumbnailDelegate = nullptr; // image/video thumbnails in icon mode
    QStackedWidget *m_bodyStack = nullptr;     // {list view, icon view}
    QToolButton *m_addTabButton; // "+" at the right end of the tab strip
    QLineEdit *m_filterBar;
    FileListView *m_view;
    StatusBarWidget *m_statusBar;
    FileSystemModel *m_model;
    QVector<NavEntry> m_backHistory;
    QVector<NavEntry> m_forwardHistory;

    TabManager *m_tabManager;
    TabBar *m_tabBar;
    QStringList m_pendingSelection;

    // Non-null while this panel is browsing inside an archive (read-only). Held
    // so the ArchiveProvider outlives the model's use of it; cleared on exit.
    std::shared_ptr<FileProvider> m_archiveProvider;
    // File name (with extension) of the archive currently browsed as a folder;
    // used as the tab label in place of the "/" virtual root. Empty when not in
    // an archive.
    QString m_archiveName;

    // Whether archives open as browsable folders (config preference). Default on.
    bool m_archiveAsFolder = true;

    // The flat search-result set currently displayed (empty when not in flat
    // mode); mirrors the model's flat entries so currentLocation() can snapshot
    // it -- the model clears its rootPath in flat mode so we can't recover it there.
    QStringList m_flatPaths;

    // Explicit thumbnail icon px set via the -/+ buttons; 0 = auto (derive from
    // m_lastFontPt). Overrides applyThumbnailFontSize()'s font-based sizing.
    int m_thumbIconSize = 0;
    // Last point size passed to applyThumbnailFontSize(), so setThumbnailIconSize()
    // (called by MainWindow before/independent of a font change) can re-derive
    // the delegate's label size without needing the caller to know it.
    int m_lastFontPt = 12;
    // Explicit list row height set via the -/+ buttons; 0 = auto.
    int m_listRowHeightOverride = 0;

    // Timer-based inline rename for a single click on an already-selected
    // thumbnail, mirroring FileListView's mouse handling for the list view (the
    // icon view has no equivalent of its own since it isn't a FileListView).
    QPersistentModelIndex m_iconRenameClickIndex;
    QTimer *m_iconRenameClickTimer = nullptr;
};
