#pragma once

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>
#include <memory>

#include "FileInfo.h"

class FileProvider;
class NetworkSession;
class DirectoryChangeMonitor;

// Flat listing of a single directory's contents (not a recursive tree) --
// this backs one FilePanel's file list. Loads asynchronously so opening a
// large directory never blocks the UI thread.
class FileSystemModel : public QAbstractTableModel {
    Q_OBJECT

public:
    // How a byte count is shown in the Size column.
    //
    // Public because it is the model's observable behaviour, not an internal
    // detail: a test that wants to know what a row displays either calls this
    // or keeps its own copy of the same arithmetic, and the copy is the thing
    // that goes stale. One did -- it assumed "<n> B" always, which holds only
    // below 1024 and made a Linux-only assertion unreachable for as long as the
    // wait in front of it was silently giving up.
    static QString formatSize(qint64 bytes);

    enum Column {
        NameColumn = 0,
        ExtColumn,
        SizeColumn,
        ModifiedColumn,
        TypeColumn,
        CreatedColumn,
        PermissionsColumn,
        ColumnCount
    };

    // Human-readable file-type category (Folder/Image/Video/Archive/...), by
    // extension. Static for reuse/testing.
    static QString typeCategory(const FileInfo &info);

    enum Role {
        FileInfoRole = Qt::UserRole + 1,
        IsDirRole,
    };

    // Result of a directory comparison, used to colour rows.
    enum CompareStatus { CompareNone, CompareUnique, CompareNewer, CompareOlder };

    explicit FileSystemModel(QObject *parent = nullptr);
    ~FileSystemModel() override;

    void setRootPath(const QString &path);
    // Re-lists the current directory asynchronously. This is intentionally the
    // same path used for F5 so provider metadata and sorting stay consistent.
    void refresh();
    QString rootPath() const { return m_rootPath; }
    quint64 loadGeneration() const { return m_loadGeneration; }
    // True when an external event was observed but has not yet been reconciled.
    bool needsExternalRefresh() const;
    // Requests a debounced background reconciliation. Safe to call before an
    // operation; it never blocks waiting for the listing.
    void refreshForExternalChange();
    // Refreshes remote sessions or a local listing whose native watch failed.
    void refreshForActivation();
    // Cheap operation preflight: only consumes an already-pending external
    // refresh. The worker remains authoritative for the real operation result.
    void refreshBeforeOperation();

    // Network connection lifecycle. Wraps `provider` in a NetworkSession running
    // on its own worker thread and starts an asynchronous connect via the
    // protocol-specific `connectFn` closure, so the GUI thread never blocks on a
    // slow/stalled link. On success the model lists `initialPath`. Connect and
    // reconnect progress is surfaced through networkStateChanged() for the status
    // line. Replaces the old synchronous setProvider()+navigate for network tabs.
    void connectNetwork(std::shared_ptr<FileProvider> provider,
                        std::function<bool(QString *)> connectFn, const QString &initialPath);
    bool hasNetworkSession() const { return m_session != nullptr; }
    // True only once the active session's link is actually established. Reads the
    // session state without taking any provider lock, so it is safe (and cheap)
    // to call from the GUI thread even while a connect is in flight -- used to
    // avoid blocking provider calls (e.g. displayName()) during connection.
    bool isConnected() const;
    // The active session's raw NetworkSession::State (-1 if no session). Lock-free;
    // drives the per-tab connection status badge on the tab icon.
    int sessionState() const;
    // User-initiated retry after the "multiple reconnects failed" state.
    void retryNetwork();

    // Given the user's credentials, produces a fresh connect closure for the same
    // target. Set by the connect site (which knows the host/protocol) so an
    // anonymous connection that turns out to need a password can be retried.
    using AuthRetryFactory =
        std::function<std::function<bool(QString *)>(const QString &user, const QString &pass)>;
    // Records who is being connected (for the prompt) and how to rebuild the
    // connect with credentials. Call right after connectNetwork().
    void setAuthContext(const QString &label, AuthRetryFactory factory);
    // Supplies the credentials the user entered after networkAuthRequired: rebuilds
    // the connect closure and retries, then re-lists once connected.
    void provideCredentials(const QString &user, const QString &pass);

    // A tab's live remote connection, moved between the model (the active tab's
    // backend) and per-tab storage so each tab keeps its OWN connection: switching
    // tabs swaps the active connection instead of one panel-wide provider. An
    // empty bundle (null session) means a local tab.
    struct NetworkConn {
        std::shared_ptr<FileProvider> provider;
        std::shared_ptr<NetworkSession> session;
        QString label;            // "user@host" for the credential prompt
        AuthRetryFactory authRetry;
        QString connectionId;     // see connectionId(); moves with the connection
    };
    // Removes the currently active network connection from the model (leaving it
    // local) and returns it so the caller can stash it with its tab. The session
    // keeps running -- it is NOT stopped -- so the connection stays warm in the
    // background. Returns an empty bundle if the active tab was already local.
    // Copies the active connection WITHOUT removing it from the model (both the
    // model and the caller then co-own the still-running session). Used to record
    // a connection into navigation history so Back can restore it. Empty bundle if
    // the active tab is local.
    NetworkConn peekConnection() const;
    NetworkConn detachConnection();
    // Installs a previously-detached connection as the active backend (or goes
    // local when the bundle is empty). Reflects the session's current state on the
    // status line immediately, and subscribes this model to the session -- which
    // matters when the connection arrives from another model (panel swap), since
    // its results must now be delivered here.
    void attachConnection(NetworkConn conn);

    // "Flat" listing mode: populate the model with an explicit set of file paths
    // that may span many directories (e.g. Ctrl+F search results shown TC
    // "feed-to-listbox" style) instead of scanning a single directory. There is
    // no ".." entry and the listing is read-only. Any subsequent setRootPath()
    // (normal navigation / refresh) leaves flat mode and restores directory
    // scanning. The Name column shows the full path so cross-directory results
    // stay distinguishable.
    void setFlatEntries(const QStringList &paths);
    bool isFlatMode() const { return m_flatMode; }

    // The backend this model reads through. Defaults to the local filesystem;
    // MainWindow/FilePanel swap in a remote provider when navigating there.
    void setProvider(std::shared_ptr<FileProvider> provider);
    FileProvider *provider() const { return m_provider.get(); }
    // Shared owner of the current provider, so a worker task (e.g. a recursive
    // directory-size walk) can keep it alive even if the model swaps providers.
    std::shared_ptr<FileProvider> providerPtr() const { return m_provider; }

    // Stable identity of the active remote connection ("sftp://user@host"), or
    // an empty string on a local/archive tab. Snapshotted once when the link
    // comes up, so reading it is a plain member access -- callers on the GUI
    // thread (e.g. per-cell painting) must NOT call provider->displayName()
    // themselves, which takes the provider's mutex and would block for the
    // whole of a slow connect. Survives navigation within the connection, so it
    // is usable as a cache key for per-file remote data.
    QString connectionId() const { return m_connectionId; }
    bool showHiddenFiles() const { return m_showHidden; }
    void setShowHiddenFiles(bool show);

    // Incremental "quick filter": restricts the visible listing to entries
    // whose name matches (case-insensitive substring, or glob if the filter
    // contains * / ?). Empty string shows everything. The ".." entry is never
    // filtered out. Cleared automatically whenever the directory changes.
    void setNameFilter(const QString &filter);
    QString nameFilter() const { return m_nameFilter; }
    static bool matchesFilter(const QString &name, const QString &filter);

    FileInfo fileInfoAt(int row) const;
    bool isParentEntry(int row) const;

    // Removes the given absolute paths from the listing incrementally (via
    // beginRemoveRows, no directory rescan) so the view keeps its scroll
    // position and can select the row that slid into the gap. Returns the
    // smallest visible row index that was removed (the "select next" anchor),
    // or -1 if nothing matched. Used after a delete instead of a full refresh.
    int removePaths(const QStringList &paths);

    // On-demand directory size: once computed (see directorySize()), the Size
    // column shows the recursive byte total for that folder instead of
    // "<DIR>". Cleared automatically when the directory is rescanned.
    void setComputedDirSize(const QString &path, qint64 bytes);
    void setDirectorySizeCalculating(const QString &path, bool calculating);
    void clearDirectorySizeCalculating();
    static qint64 directorySize(const QString &path);

    // Colours rows per a name->CompareStatus map (from "Compare Directories").
    // Cleared automatically on rescan.
    void setCompareStatus(const QHash<QString, int> &statusByName);
    void clearCompareStatus();

    // Re-emits header + cell change signals so views re-query tr()'d text (column
    // titles, the type column) after a live UI-language switch. No rescan.
    void retranslate();

    // Pure comparison: each name in `self` is Unique (absent from `other`),
    // Newer, or Older based on modification times. Static for unit testing.
    static QHash<QString, int> compareStatuses(const QHash<QString, QDateTime> &self,
                                                const QHash<QString, QDateTime> &other);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

signals:
    void loadStarted();
    void loadFinished(int count, quint64 generation);
    // Emitted immediately before a model-triggered external reload. FilePanel
    // uses it to preserve the current row/selection across the reset.
    void externalRefreshStarted();
    void renameFailed(const QString &message);
    void renamed(const QString &oldPath, const QString &newPath);
    // An inline rename on a REMOTE tab: the actual provider->rename() is a
    // blocking round trip (SFTP does stat + rename), so setData refuses to run it
    // on the GUI thread and asks the owner (MainWindow) to enqueue it on the
    // transfer pool instead. The panel refreshes on completion; a failure is
    // surfaced by the queue's error path (the row was never optimistically
    // changed, so nothing to roll back).
    void remoteRenameRequested(const QString &oldPath, const QString &newName);
    // Network connection state for the status line: state is a
    // NetworkSession::State, attempt is the current reconnect attempt (1..N).
    void networkStateChanged(int state, int attempt);
    // The connection adopted an OS-level session that was already open, so the
    // browse runs under `user` rather than the requested credentials. Purely
    // informational; the link is up.
    void networkSessionReused(const QString &user);
    // A listing failed for a reason the backend could name -- distinct from the
    // connection itself failing, which networkStateChanged already covers. The
    // link may well still be up (a share the server declines to enumerate), so
    // the panel reports this without declaring the connection dead.
    void listingFailed(const QString &reason);
    // The server needs credentials: `label` (host) identifies which, `error` is
    // the server's reason (may be empty; distinguishes wrong-password from a
    // first-time prompt). The UI should prompt and call provideCredentials().
    void networkAuthRequired(const QString &label, const QString &error);

public:
    // The active session's last connection-failure reason (empty if none/cleared),
    // for a specific "connection failed: <reason>" status line. Cleared on a fresh
    // connect and on a successful (re)connect.
    QString lastNetworkError() const { return m_lastNetworkError; }

private slots:
    void onScanFinished();
    void onSessionListReady(quint64 reqId, const QString &path, const QVector<FileInfo> &entries);
    void onSessionListFailed(quint64 reqId, const QString &path, const QString &reason);
    void onSessionAuthRequired(const QString &error);
    void onSessionFailed(const QString &error);
    void onSessionStateChanged(int state, int attempt);
    void onExternalChangesDetected();
    void onExternalReconciliationRequired();
    void onExternalRefreshTimeout();

private:
    struct LocalScanResult {
        quint64 generation = 0;
        QString path;
        QVector<FileInfo> entries;
    };

    // The single point where m_provider is assigned, so the cached
    // m_virtualListing flag can never fall out of step with it.
    void installProvider(std::shared_ptr<FileProvider> provider);
    // (Re)subscribes this model to m_session's signals. Idempotent, so adopting
    // the same session repeatedly cannot stack duplicate deliveries.
    void wireSessionSignals();
    void teardownSession();
    void sortEntries();
    void applyFilter();
    // Decides whether a listing at `path` gets a ".." row and, if so, builds it
    // through the active provider. Call wherever the listing is replaced.
    void setParentEntryFor(const QString &path);
    void scheduleExternalRefresh();
    // Formats a timestamp as "yyyy-MM-dd HH:mm", memoised by epoch-minute.
    QString cachedDateStr(const QDateTime &dt) const;

    std::shared_ptr<FileProvider> m_provider; // backend (local by default); never null
    // Cache of m_provider->isVirtualListing(); see installProvider(). data() is
    // called several times per cell per repaint, so the three synthetic-listing
    // hooks must not cost a virtual call on every real backend.
    bool m_virtualListing = false;
    QString m_rootPath;
    bool m_flatMode = false; // true while showing an explicit cross-directory path list
    bool m_showHidden = false;
    QVector<FileInfo> m_allEntries; // full directory scan (source of truth)
    QVector<FileInfo> m_entries;    // visible subset after quick filter
    QString m_nameFilter;
    QHash<QString, qint64> m_dirSizes;    // path -> computed recursive size
    QSet<QString> m_calculatingDirSizes;  // paths whose slow size task is still running
    QHash<QString, int> m_compareStatus;  // name -> CompareStatus
    // Memoises the formatted "yyyy-MM-dd HH:mm" string per epoch-minute so the
    // (expensive) QDateTime::toString runs once per distinct timestamp instead
    // of once per cell per repaint. Pure function of the value, so it only needs
    // clearing to bound its size (done at each directory scan).
    mutable QHash<qint64, QString> m_dateStrCache;
    bool m_hasParentEntry = false;
    // The ".." row, built once per listing by setParentEntryFor(). It used to be
    // constructed on the spot by every query that touched row 0 -- and on a
    // local backend that constructor is a stat(), so a single repaint ran about
    // twenty of them on the GUI thread. Harmless on a local disk, but enough to
    // freeze the window on a wedged autofs/NFS mount.
    FileInfo m_parentEntry;
    QFutureWatcher<LocalScanResult> m_watcher;
    DirectoryChangeMonitor *m_directoryMonitor = nullptr;
    QTimer m_externalRefreshTimer;
    bool m_externalRefreshPending = false;
    bool m_startingDirectoryWatch = false;
    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    quint64 m_loadGeneration = 0;

    // Network tab: worker-thread session owning the network provider. Null for
    // local/archive tabs (which keep the QtConcurrent scan path above).
    std::shared_ptr<NetworkSession> m_session;
    quint64 m_reqId = 0; // monotonic list request id; stale results ignored
    AuthRetryFactory m_authRetry;   // rebuilds the connect with credentials
    QString m_networkLabel;         // host shown in the credentials prompt
    QString m_lastNetworkError;     // last connect-failure reason (for the status line)
    QString m_connectionId;         // "scheme://user@host" once connected; see connectionId()
    bool m_relistOnConnect = false; // re-list rootPath once a credentialed retry lands
};
