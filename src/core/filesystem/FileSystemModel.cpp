#include "FileSystemModel.h"

#include <QCollator>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrent>

#include "FileProvider.h"
#include "IconCache.h"
#include "LocalFileProvider.h"
#include "DirectoryChangeMonitor.h"
#include "network/NetworkSession.h"

QString FileSystemModel::formatSize(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

namespace {
// The local provider is a process-wide singleton; wrap it in a shared_ptr with
// a no-op deleter so the model can hold every provider uniformly by shared_ptr.
std::shared_ptr<FileProvider> localProviderPtr() {
    return std::shared_ptr<FileProvider>(LocalFileProvider::instance(), [](FileProvider *) {});
}
} // namespace

FileSystemModel::FileSystemModel(QObject *parent) : QAbstractTableModel(parent) {
    installProvider(localProviderPtr());
    m_directoryMonitor = new DirectoryChangeMonitor(this);
    m_externalRefreshTimer.setSingleShot(true);
    m_externalRefreshTimer.setInterval(200);
    connect(&m_externalRefreshTimer, &QTimer::timeout, this,
            &FileSystemModel::onExternalRefreshTimeout);
    connect(m_directoryMonitor, &DirectoryChangeMonitor::changesDetected, this,
            &FileSystemModel::onExternalChangesDetected);
    connect(m_directoryMonitor, &DirectoryChangeMonitor::reconciliationRequired, this,
            &FileSystemModel::onExternalReconciliationRequired);
    connect(&m_watcher, &QFutureWatcher<LocalScanResult>::finished, this,
            &FileSystemModel::onScanFinished);
}

FileSystemModel::~FileSystemModel() = default; // out-of-line: completes shared_ptr<NetworkSession>

void FileSystemModel::setProvider(std::shared_ptr<FileProvider> provider) {
    // Switching to a different (or local) provider abandons any network session.
    if (m_directoryMonitor)
        m_directoryMonitor->stopWatching();
    m_externalRefreshTimer.stop();
    m_externalRefreshPending = false;
    if (m_session && (!provider || provider.get() != m_session->provider()))
        teardownSession();
    installProvider(provider ? std::move(provider) : localProviderPtr());
}

void FileSystemModel::installProvider(std::shared_ptr<FileProvider> provider) {
    m_provider = std::move(provider);
    // Snapshotted rather than asked per cell: data() runs several times per cell
    // on every repaint, and the answer cannot change under a provider. Every
    // assignment to m_provider goes through here so the two cannot drift.
    m_virtualListing = m_provider->isVirtualListing();
}

void FileSystemModel::teardownSession() {
    if (!m_session)
        return;
    m_session->stop();
    m_session.reset(); // last drop -> shutdownAsync tears down without blocking
}

void FileSystemModel::connectNetwork(std::shared_ptr<FileProvider> provider,
                                     std::function<bool(QString *)> connectFn,
                                     const QString &initialPath) {
    if (m_directoryMonitor)
        m_directoryMonitor->stopWatching();
    m_externalRefreshTimer.stop();
    m_externalRefreshPending = false;
    teardownSession();
    installProvider(provider); // network provider becomes the backend
    // Custom deleter: the last owner drop tears the session down asynchronously
    // (shutdownAsync) so closing a tab / swapping a session never blocks the GUI
    // on a stalled worker's join. Every copy (parked in tabs/history) shares it.
    m_session = std::shared_ptr<NetworkSession>(new NetworkSession(provider),
                                                [](NetworkSession *s) { s->shutdownAsync(); });
    wireSessionSignals();
    m_authRetry = nullptr;   // connect site sets it via setAuthContext for retryable backends
    m_lastNetworkError.clear();
    m_connectionId.clear();  // filled in once the link is up (see onSessionStateChanged)
    m_relistOnConnect = false;
    // Start connecting on the worker thread. The caller then navigates to
    // initialPath: that requestList is queued after this connect on the single
    // worker thread, so it lists only once the connection lands (no double-list).
    m_session->start(std::move(connectFn), initialPath);
}

FileSystemModel::NetworkConn FileSystemModel::peekConnection() const {
    NetworkConn c;
    if (m_session) {
        c.provider = m_provider;
        c.session = m_session;
        c.label = m_networkLabel;
        c.authRetry = m_authRetry;
        c.connectionId = m_connectionId;
    }
    return c;
}

FileSystemModel::NetworkConn FileSystemModel::detachConnection() {
    NetworkConn c;
    if (m_session) {
        // Hand the live connection to the caller WITHOUT stopping it: it stays
        // alive (and its heartbeat keeps reconnecting) while parked with its tab.
        //
        // Drop this model's subscription as it goes. Parking within one model
        // could rely on the sender() guards in the slots, since the connection
        // always came back to the same object. A connection that moves to a
        // *different* model cannot: both would stay subscribed, and the model
        // that no longer owns it would still act on its listings.
        disconnect(m_session.get(), nullptr, this, nullptr);
        c.provider = m_provider;
        c.session = m_session;
        c.label = m_networkLabel;
        c.authRetry = m_authRetry;
        c.connectionId = m_connectionId;
        m_session.reset();
        m_provider = localProviderPtr();
        m_networkLabel.clear();
        m_authRetry = nullptr;
        m_connectionId.clear();
        m_relistOnConnect = false;
    }
    return c;
}

void FileSystemModel::wireSessionSignals() {
    if (!m_session)
        return;
    // Re-pointed at whichever model currently owns the session: a connection can
    // move between models (swapping the two panels), and its results have to
    // arrive at the model that is now showing it. Qt::UniqueConnection keeps
    // re-adopting the same session from stacking duplicates.
    connect(m_session.get(), &NetworkSession::stateChanged, this,
            &FileSystemModel::onSessionStateChanged, Qt::UniqueConnection);
    connect(m_session.get(), &NetworkSession::listReady, this,
            &FileSystemModel::onSessionListReady, Qt::UniqueConnection);
    connect(m_session.get(), &NetworkSession::listFailed, this,
            &FileSystemModel::onSessionListFailed, Qt::UniqueConnection);
    connect(m_session.get(), &NetworkSession::authRequired, this,
            &FileSystemModel::onSessionAuthRequired, Qt::UniqueConnection);
    connect(m_session.get(), &NetworkSession::reusedExistingSession, this,
            [this](const QString &user) {
                if (sender() == m_session.get())
                    emit networkSessionReused(user);
            },
            Qt::UniqueConnection);
    connect(m_session.get(), &NetworkSession::failed, this, &FileSystemModel::onSessionFailed,
            Qt::UniqueConnection);
}

void FileSystemModel::attachConnection(NetworkConn conn) {
    if (m_directoryMonitor)
        m_directoryMonitor->stopWatching();
    m_externalRefreshTimer.stop();
    m_externalRefreshPending = false;
    if (conn.session) {
        installProvider(conn.provider);
        m_session = conn.session;
        m_networkLabel = conn.label;
        m_authRetry = conn.authRetry;
        m_connectionId = conn.connectionId;
        m_relistOnConnect = false;
        // Subscribe before anything else: the session may have come from another
        // model, whose subscription was dropped on detach, so without this its
        // listings would arrive nowhere and the panel would never finish loading.
        wireSessionSignals();
        // Bring the status line up to date with this connection's current state
        // (the session won't re-emit on its own just because we re-adopted it).
        emit networkStateChanged(m_session->state(), m_session->attempt());
    } else {
        // Local tab: drop to the local provider, no session.
        m_session.reset();
        installProvider(localProviderPtr());
        m_networkLabel.clear();
        m_authRetry = nullptr;
        m_connectionId.clear();
    }
    m_lastNetworkError.clear(); // belongs to the previous active connection
}

bool FileSystemModel::isConnected() const {
    return m_session && m_session->state() == NetworkSession::Connected;
}

int FileSystemModel::sessionState() const {
    return m_session ? static_cast<int>(m_session->state()) : -1;
}

void FileSystemModel::setAuthContext(const QString &label, AuthRetryFactory factory) {
    m_networkLabel = label;
    m_authRetry = std::move(factory);
}

void FileSystemModel::onSessionAuthRequired(const QString &error) {
    if (sender() != m_session.get())
        return; // parked background session; ignore
    m_lastNetworkError = error;
    emit networkAuthRequired(m_networkLabel, error); // UI prompts, then provideCredentials
}

void FileSystemModel::onSessionFailed(const QString &error) {
    if (sender() != m_session.get())
        return; // parked background session; ignore
    m_lastNetworkError = error; // surfaced by the status line when Failed arrives
}

void FileSystemModel::provideCredentials(const QString &user, const QString &pass) {
    if (!m_session || !m_authRetry)
        return;
    m_relistOnConnect = true; // navigateTo's list request was consumed by the failed attempt
    m_session->retryWith(m_authRetry(user, pass));
}

void FileSystemModel::retryNetwork() {
    if (m_session)
        m_session->retry();
}

void FileSystemModel::setRootPath(const QString &path) {
    if (m_directoryMonitor)
        m_directoryMonitor->stopWatching();
    m_externalRefreshTimer.stop();
    m_externalRefreshPending = false;
    ++m_loadGeneration;
    m_flatMode = false;       // leaving any flat search-results listing
    m_rootPath = path;
    m_nameFilter.clear();     // a fresh directory always starts unfiltered
    m_dirSizes.clear();       // computed folder sizes don't survive a rescan
    m_calculatingDirSizes.clear();
    m_compareStatus.clear();  // comparison highlights are stale after a rescan
    m_dateStrCache.clear();   // bound the memo; new listing, new timestamps
    emit loadStarted();

    // Only a real local directory has a native change stream. Archive and
    // synthetic providers deliberately fall through to their existing listing
    // path; remote tabs are reconciled by activation instead.
    if (m_directoryMonitor && m_provider && m_provider->isLocalFilesystem() &&
        !m_virtualListing && !path.isEmpty()) {
        m_startingDirectoryWatch = true;
        m_directoryMonitor->startWatching(path);
        m_startingDirectoryWatch = false;
    }

    // Network tab: route the listing through the session's worker thread. Once
    // connected, the current view is left intact until the result arrives so a
    // slow refresh never blanks the panel (no flicker). Stale results are
    // discarded by reqId. But while the link is NOT yet up (connecting /
    // reconnecting / failed), blank the listing first: otherwise the panel would
    // keep showing whatever was there before -- typically a LOCAL directory --
    // under a remote tab, which looks browsable but isn't.
    if (m_session) {
        if (!isConnected()) {
            beginResetModel();
            m_allEntries.clear();
            setParentEntryFor(QString()); // nothing listed yet: no ".." either
            sortEntries();
            endResetModel();
            emit loadFinished(m_entries.size(), m_loadGeneration);
        }
        m_session->requestList(++m_reqId, path, m_showHidden);
        return;
    }

    // Local/archive: scan on a QtConcurrent worker as before. Capture a
    // shared_ptr copy so the provider outlives this scan even if the model
    // switches providers meanwhile.
    std::shared_ptr<FileProvider> provider = m_provider;
    const bool showHidden = m_showHidden;
    const quint64 generation = m_loadGeneration;
    QFuture<LocalScanResult> future = QtConcurrent::run([provider, path, showHidden, generation] {
        return LocalScanResult{generation, path, provider->list(path, showHidden)};
    });
    m_watcher.setFuture(future);
}

void FileSystemModel::refresh() {
    if (!m_rootPath.isEmpty() && !m_flatMode)
        setRootPath(m_rootPath);
}

bool FileSystemModel::needsExternalRefresh() const {
    return m_externalRefreshPending ||
           (m_directoryMonitor && m_directoryMonitor->needsReconciliation());
}

void FileSystemModel::refreshForExternalChange() {
    if (m_rootPath.isEmpty() || m_flatMode)
        return;
    scheduleExternalRefresh();
}

void FileSystemModel::refreshForActivation() {
    if (m_rootPath.isEmpty() || m_flatMode || m_virtualListing)
        return;
    if (m_session || needsExternalRefresh())
        scheduleExternalRefresh();
}

void FileSystemModel::refreshBeforeOperation() {
    if (needsExternalRefresh())
        scheduleExternalRefresh();
}

void FileSystemModel::onExternalChangesDetected() {
    scheduleExternalRefresh();
}

void FileSystemModel::onExternalReconciliationRequired() {
    if (m_startingDirectoryWatch)
        return;
    scheduleExternalRefresh();
}

void FileSystemModel::scheduleExternalRefresh() {
    if (m_rootPath.isEmpty() || m_flatMode || m_virtualListing)
        return;
    m_externalRefreshPending = true;
    // A scan already in flight will consume the pending flag in
    // onScanFinished(), so no second concurrent local list is started here.
    if (!m_watcher.isRunning() && !m_externalRefreshTimer.isActive())
        m_externalRefreshTimer.start();
}

void FileSystemModel::onExternalRefreshTimeout() {
    if (!m_externalRefreshPending || m_rootPath.isEmpty() || m_flatMode ||
        m_virtualListing)
        return;
    if (m_watcher.isRunning())
        return;
    m_externalRefreshPending = false;
    emit externalRefreshStarted();
    refresh();
}

void FileSystemModel::onSessionStateChanged(int state, int attempt) {
    // Ignore state changes from a backgrounded (parked) session belonging to an
    // inactive tab -- only the active tab's connection drives the status line.
    if (sender() != m_session.get())
        return;
    if (state == NetworkSession::Connected) {
        m_lastNetworkError.clear(); // a good connect clears any stale failure reason
        // Snapshot the connection's identity now, while we are on the GUI thread
        // but the provider's mutex is demonstrably free (the connect just
        // finished). Everything downstream reads the cached copy instead of
        // calling displayName() again, which would block on the next stall.
        if (m_provider)
            m_connectionId = m_provider->scheme() + QStringLiteral("://")
                             + m_provider->displayName();
    }
    // Forward to the status line. The initial listing is driven by the caller's
    // navigateTo() (queued behind the connect on the worker thread); post-drop
    // refreshes are emitted by the session itself as listReady(reqId 0).
    emit networkStateChanged(state, attempt);
    // A credentialed retry just connected: the original list request was already
    // consumed by the failed anonymous attempt, so re-list the current directory.
    if (state == NetworkSession::Connected && m_relistOnConnect) {
        m_relistOnConnect = false;
        setRootPath(m_rootPath);
    }
}

void FileSystemModel::onSessionListReady(quint64 reqId, const QString &path,
                                         const QVector<FileInfo> &entries) {
    if (sender() != m_session.get())
        return; // a parked background session's stale result; not the active tab
    if (m_flatMode)
        return;
    // reqId 0 is a session-initiated refresh (post-reconnect): accept only if it
    // still matches the current directory. Otherwise ignore superseded results.
    if (reqId == 0) {
        if (path != m_rootPath)
            return;
    } else if (reqId != m_reqId) {
        return;
    }
    beginResetModel();
    m_allEntries = entries;
    setParentEntryFor(path);
    sortEntries();
    endResetModel();
    m_rootPath = path;
    emit loadFinished(m_entries.size(), m_loadGeneration);
}

void FileSystemModel::onSessionListFailed(quint64 reqId, const QString &path,
                                          const QString &reason) {
    Q_UNUSED(path);
    if (sender() != m_session.get())
        return; // parked background session; ignore
    if (reqId != 0 && reqId != m_reqId)
        return;
    // A backend that knows why it failed gets to say so. Without this the panel
    // just stopped its spinner and left the previous (or empty) listing on
    // screen, which is how "the server refused to enumerate its shares" reached
    // the user as a blank pane and nothing else.
    if (!reason.isEmpty()) {
        m_lastNetworkError = reason;
        emit listingFailed(reason);
    }
    // Keep the current view intact (don't blank it); the status line already
    // reflects the reconnect/failed state. End any loading indicator.
    emit loadFinished(m_entries.size(), m_loadGeneration);
}

void FileSystemModel::setShowHiddenFiles(bool show) {
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    if (!m_rootPath.isEmpty())
        setRootPath(m_rootPath);
}

void FileSystemModel::setFlatEntries(const QStringList &paths) {
    if (m_directoryMonitor)
        m_directoryMonitor->stopWatching();
    m_externalRefreshTimer.stop();
    m_externalRefreshPending = false;
    ++m_loadGeneration;
    emit loadStarted();
    beginResetModel();
    m_flatMode = true;
    m_rootPath.clear();      // no single directory backs a flat listing
    m_nameFilter.clear();
    m_dirSizes.clear();
    m_compareStatus.clear();
    setParentEntryFor(QString()); // cross-directory list has no single ".." parent
    m_allEntries.clear();
    m_allEntries.reserve(paths.size());
    for (const QString &p : paths) {
        FileInfo info(p);
        if (info.isValid())
            m_allEntries.append(info);
    }
    sortEntries();           // sorts m_allEntries and rebuilds the visible m_entries
    endResetModel();
    emit loadFinished(m_entries.size(), m_loadGeneration);
}

void FileSystemModel::onScanFinished() {
    // A directory scan started before we switched to flat mode may still be
    // running; ignore its late result so it doesn't clobber the flat listing.
    if (m_flatMode)
        return;
    // A LOCAL scan (e.g. from a freshly-created tab's initial listing) may still
    // be in flight when the tab is turned into a network tab by connectNetwork().
    // Its late result is stale -- dropping it here is what keeps a connecting
    // remote tab blank instead of briefly showing the local directory.
    if (m_session)
        return;
    const LocalScanResult result = m_watcher.result();
    if (result.generation != m_loadGeneration || result.path != m_rootPath)
        return;
    beginResetModel();
    m_allEntries = result.entries;
    setParentEntryFor(result.path);
    sortEntries(); // sorts m_allEntries and rebuilds the visible m_entries
    endResetModel();
    emit loadFinished(m_entries.size(), result.generation);
    if (m_externalRefreshPending && !m_externalRefreshTimer.isActive())
        m_externalRefreshTimer.start();
}

bool FileSystemModel::matchesFilter(const QString &name, const QString &filter) {
    if (filter.isEmpty())
        return true;
    if (filter.contains(QLatin1Char('*')) || filter.contains(QLatin1Char('?'))) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(filter),
                                     QRegularExpression::CaseInsensitiveOption);
        return re.match(name).hasMatch();
    }
    return name.contains(filter, Qt::CaseInsensitive);
}

void FileSystemModel::applyFilter() {
    if (m_nameFilter.isEmpty()) {
        m_entries = m_allEntries;
        return;
    }
    m_entries.clear();
    m_entries.reserve(m_allEntries.size());
    for (const FileInfo &entry : m_allEntries)
        if (matchesFilter(entry.name(), m_nameFilter))
            m_entries.append(entry);
}

void FileSystemModel::setNameFilter(const QString &filter) {
    if (m_nameFilter == filter)
        return;
    beginResetModel();
    m_nameFilter = filter;
    applyFilter();
    endResetModel();
}

qint64 FileSystemModel::directorySize(const QString &path) {
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

void FileSystemModel::setCompareStatus(const QHash<QString, int> &statusByName) {
    beginResetModel();
    m_compareStatus = statusByName;
    endResetModel();
}

QHash<QString, int> FileSystemModel::compareStatuses(const QHash<QString, QDateTime> &self,
                                                      const QHash<QString, QDateTime> &other) {
    QHash<QString, int> result;
    for (auto it = self.constBegin(); it != self.constEnd(); ++it) {
        if (!other.contains(it.key()))
            result.insert(it.key(), CompareUnique);
        else if (it.value() > other.value(it.key()))
            result.insert(it.key(), CompareNewer);
        else
            result.insert(it.key(), CompareOlder);
    }
    return result;
}

void FileSystemModel::clearCompareStatus() {
    if (m_compareStatus.isEmpty())
        return;
    beginResetModel();
    m_compareStatus.clear();
    endResetModel();
}

void FileSystemModel::setComputedDirSize(const QString &path, qint64 bytes) {
    m_dirSizes.insert(path, bytes);
    m_calculatingDirSizes.remove(path);
    // Repaint the Size cell of the matching visible row, if it's shown.
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).path() == path) {
            const int row = m_hasParentEntry ? i + 1 : i;
            const QModelIndex idx = index(row, SizeColumn);
            emit dataChanged(idx, idx, {Qt::DisplayRole});
            break;
        }
    }
}

void FileSystemModel::setDirectorySizeCalculating(const QString &path, bool calculating) {
    if (path.isEmpty())
        return;
    const bool changed =
        calculating ? !m_calculatingDirSizes.contains(path)
                    : m_calculatingDirSizes.contains(path);
    if (!changed)
        return;
    if (calculating)
        m_calculatingDirSizes.insert(path);
    else
        m_calculatingDirSizes.remove(path);

    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).path() == path) {
            const int row = m_hasParentEntry ? i + 1 : i;
            const QModelIndex idx = index(row, SizeColumn);
            emit dataChanged(idx, idx, {Qt::DisplayRole});
            break;
        }
    }
}

void FileSystemModel::clearDirectorySizeCalculating() {
    if (m_calculatingDirSizes.isEmpty())
        return;
    const QSet<QString> paths = m_calculatingDirSizes;
    m_calculatingDirSizes.clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (paths.contains(m_entries.at(i).path())) {
            const int row = m_hasParentEntry ? i + 1 : i;
            const QModelIndex idx = index(row, SizeColumn);
            emit dataChanged(idx, idx, {Qt::DisplayRole});
        }
    }
}

bool FileSystemModel::isParentEntry(int row) const {
    return m_hasParentEntry && row == 0;
}

void FileSystemModel::setParentEntryFor(const QString &path) {
    const QString parent =
        (m_provider && !path.isEmpty()) ? m_provider->parentPath(path) : QString();
    m_hasParentEntry = !parent.isEmpty();
    // Built through the provider: only a local backend's parent path may be
    // stat'ed, and only the provider knows whether this one is (see
    // FileInfo::makeParentEntry).
    m_parentEntry = m_hasParentEntry
                        ? FileInfo::makeParentEntry(parent, m_provider->isLocalFilesystem())
                        : FileInfo();
}

FileInfo FileSystemModel::fileInfoAt(int row) const {
    if (isParentEntry(row))
        return m_parentEntry;
    int idx = m_hasParentEntry ? row - 1 : row;
    if (idx < 0 || idx >= m_entries.size())
        return FileInfo();
    return m_entries.at(idx);
}

int FileSystemModel::removePaths(const QStringList &paths) {
    if (paths.isEmpty())
        return -1;
    const QSet<QString> targets(paths.begin(), paths.end());
    int anchorRow = -1;
    // Walk the visible list back-to-front so earlier row indices stay valid as
    // we splice each match out. The last (lowest) row we touch is the anchor.
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (!targets.contains(m_entries.at(i).path()))
            continue;
        const int row = m_hasParentEntry ? i + 1 : i;
        beginRemoveRows(QModelIndex(), row, row);
        m_entries.remove(i);
        endRemoveRows();
        anchorRow = row;
    }
    // Keep the unfiltered backing store in sync so a later sort/filter/rescan
    // boundary doesn't resurrect the removed entries.
    for (int i = m_allEntries.size() - 1; i >= 0; --i)
        if (targets.contains(m_allEntries.at(i).path()))
            m_allEntries.remove(i);
    return anchorRow;
}

int FileSystemModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return m_entries.size() + (m_hasParentEntry ? 1 : 0);
}

int FileSystemModel::columnCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QString FileSystemModel::cachedDateStr(const QDateTime &dt) const {
    if (!dt.isValid())
        return {};
    // Key by whole minutes -- the format has no seconds, so timestamps in the
    // same minute share a string.
    const qint64 key = dt.toMSecsSinceEpoch() / 60000;
    auto it = m_dateStrCache.constFind(key);
    if (it != m_dateStrCache.constEnd())
        return it.value();
    const QString s = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    m_dateStrCache.insert(key, s);
    return s;
}

QVariant FileSystemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};

    // Hot path: data() is called several times per cell on every repaint, so
    // bind a reference into m_entries instead of copying a (QString/QDateTime-
    // heavy) FileInfo each time. Only the rare parent ".." row needs a
    // constructed value.
    const int row = index.row();
    const FileInfo *infoPtr;
    if (isParentEntry(row)) {
        infoPtr = &m_parentEntry;
    } else {
        const int idx = m_hasParentEntry ? row - 1 : row;
        if (idx < 0 || idx >= m_entries.size())
            return {};
        infoPtr = &m_entries.at(idx);
    }
    const FileInfo &info = *infoPtr;
    if (!info.isValid())
        return {};

    if (role == FileInfoRole)
        return QVariant::fromValue(info.path());
    if (role == IsDirRole)
        return info.isDir();
    if (role == Qt::DecorationRole && index.column() == NameColumn) {
        if (m_virtualListing) {
            // A drive DOES have something the system can be asked about, and it
            // gives each volume its own icon (fixed, removable, optical, a
            // mapped share), so that beats our one generic disk glyph. Tried
            // first, and falls through when the platform has no answer.
            const QString systemPath = m_provider->entrySystemIconPath(info.path());
            if (!systemPath.isEmpty()) {
                const QIcon icon = IconCache::instance().systemIconForPath(systemPath);
                if (!icon.availableSizes().isEmpty())
                    return icon;
            }
            // A server has no file on disk to take an icon from, so the backend
            // names one. An empty answer means "no opinion" and the normal
            // resolution below applies -- which is what the synthetic folder
            // rows want.
            const QString iconPath = m_provider->entryIconPath(info.path());
            if (!iconPath.isEmpty())
                return IconCache::instance().glyphIcon(iconPath);
        }
        return IconCache::instance().iconFor(info);
    }

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case NameColumn:
            // Flat search-results listing shows the full path so results from
            // different directories stay distinguishable; a normal directory
            // listing shows the base name only (extension is its own column).
            return m_flatMode ? info.path() : info.baseName();
        case ExtColumn:
            return info.isDir() ? QString() : info.suffix();
        case SizeColumn:
            if (m_virtualListing) {
                const QString text = m_provider->entrySizeText(info.path());
                if (!text.isEmpty())
                    return text;
            }
            if (info.isDir()) {
                auto it = m_dirSizes.constFind(info.path());
                if (it != m_dirSizes.constEnd())
                    return FileSystemModel::formatSize(it.value());
                if (m_calculatingDirSizes.contains(info.path()))
                    return QObject::tr("calculating");
                return QStringLiteral("<DIR>");
            }
            return FileSystemModel::formatSize(info.size());
        case ModifiedColumn:
            return cachedDateStr(info.modified());
        case CreatedColumn:
            return info.created().isValid() ? cachedDateStr(info.created()) : QString();
        case TypeColumn:
            if (m_virtualListing) {
                const QString label = m_provider->entryTypeLabel(info.path());
                if (!label.isEmpty())
                    return label;
            }
            return typeCategory(info);
        case PermissionsColumn:
            return info.permissionsString();
        default:
            return {};
        }
    }

    if (role == Qt::TextAlignmentRole && index.column() == SizeColumn)
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);

    if (role == Qt::ForegroundRole && !m_compareStatus.isEmpty()) {
        switch (m_compareStatus.value(info.name(), CompareNone)) {
        case CompareNewer:
            return QColor(0xc0, 0x39, 0x2b); // red: newer than the other panel
        case CompareUnique:
            return QColor(0x27, 0x7a, 0x46); // green: only on this side
        default:
            break;
        }
    }

    return {};
}

void FileSystemModel::retranslate() {
    // Column titles are tr()'d in headerData(); the type column's text is tr()'d
    // in data(). Re-emit so the views re-query both in the new language.
    emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
    if (!m_entries.isEmpty())
        emit dataChanged(index(0, 0), index(m_entries.size() - 1, columnCount() - 1),
                         {Qt::DisplayRole});
}

QVariant FileSystemModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);

    switch (section) {
    case NameColumn:
        return QObject::tr("Name");
    case ExtColumn:
        return QObject::tr("Ext");
    case SizeColumn:
        return QObject::tr("Size");
    case ModifiedColumn:
        return QObject::tr("Modified");
    case CreatedColumn:
        return QObject::tr("Created");
    case TypeColumn:
        return QObject::tr("Type");
    case PermissionsColumn:
        return QObject::tr("Permissions");
    default:
        return {};
    }
}

QString FileSystemModel::typeCategory(const FileInfo &info) {
    if (info.isDir())
        return QObject::tr("Folder");
    const QString ext = info.suffix().toLower();
    if (ext.isEmpty())
        return QObject::tr("File");

    static const QSet<QString> images = {"jpg", "jpeg", "png",  "gif",  "bmp", "webp",
                                          "svg", "tiff", "tif",  "ico",  "heic"};
    static const QSet<QString> videos = {"mp4", "mkv",  "avi",  "mov", "wmv",
                                          "flv", "webm", "m4v",  "mpg", "mpeg", "ts"};
    static const QSet<QString> audios = {"mp3", "wav", "flac", "ogg", "aac", "m4a", "wma", "opus"};
    static const QSet<QString> archives = {"zip", "rar", "7z",  "tar", "gz",
                                            "bz2", "xz",  "tgz", "zst", "lz"};
    static const QSet<QString> docs = {"doc", "docx", "pdf", "txt", "md",   "odt", "xls",
                                        "xlsx", "ppt", "pptx", "rtf", "csv", "epub"};
    if (images.contains(ext))
        return QObject::tr("Image");
    if (videos.contains(ext))
        return QObject::tr("Video");
    if (audios.contains(ext))
        return QObject::tr("Audio");
    if (archives.contains(ext))
        return QObject::tr("Archive");
    if (docs.contains(ext))
        return QObject::tr("Document");
    return ext.toUpper(); // e.g. "PY", "SH"
}

void FileSystemModel::sort(int column, Qt::SortOrder order) {
    m_sortColumn = column;
    m_sortOrder = order;
    beginResetModel();
    sortEntries();
    endResetModel();
}

void FileSystemModel::sortEntries() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    std::sort(m_allEntries.begin(), m_allEntries.end(), [&](const FileInfo &a, const FileInfo &b) {
        // Directories always sort before files, regardless of sort column.
        if (a.isDir() != b.isDir())
            return a.isDir();

        // A synthetic listing is grouped into sections (drives, then user
        // folders, then servers). The sections hold their order under every
        // column and both directions -- sorting by size must not interleave a
        // network host between two drives -- so this is decided before, and
        // independently of, the user's sort choice.
        if (m_virtualListing) {
            const int groupA = m_provider->entrySortGroup(a.path());
            const int groupB = m_provider->entrySortGroup(b.path());
            if (groupA != groupB)
                return groupA < groupB;
        }

        int cmp = 0;
        switch (m_sortColumn) {
        case SizeColumn: {
            // Use a computed folder size where we have one, so sorting by size
            // orders directories by their real footprint, not the tiny inode.
            auto effectiveSize = [this](const FileInfo &fi) {
                auto it = m_dirSizes.constFind(fi.path());
                return it != m_dirSizes.constEnd() ? it.value() : fi.size();
            };
            const qint64 sa = effectiveSize(a);
            const qint64 sb = effectiveSize(b);
            cmp = (sa < sb) ? -1 : (sa > sb ? 1 : 0);
            break;
        }
        case ModifiedColumn:
            cmp = a.modified() < b.modified() ? -1 : (a.modified() > b.modified() ? 1 : 0);
            break;
        case CreatedColumn:
            cmp = a.created() < b.created() ? -1 : (a.created() > b.created() ? 1 : 0);
            break;
        case TypeColumn:
            cmp = collator.compare(typeCategory(a), typeCategory(b));
            break;
        case ExtColumn:
            cmp = collator.compare(a.suffix(), b.suffix());
            break;
        default:
            cmp = collator.compare(a.name(), b.name());
            break;
        }
        if (cmp == 0)
            cmp = collator.compare(a.name(), b.name());
        return m_sortOrder == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
    });
    applyFilter(); // keep the visible subset in sync with the sorted source
}

Qt::ItemFlags FileSystemModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (!index.isValid())
        return f;
    // Flat search-results listing is read-only: its entries live in many
    // different directories, so inline rename and drop-target semantics don't
    // apply. Dragging results out is still useful, so keep that.
    if (m_flatMode) {
        f |= Qt::ItemIsDragEnabled;
        return f;
    }
    // Account-device rows remain selectable while offline so users can inspect
    // their status; MainWindow reports the offline condition on activation.    // ".."), and on the Ext cell of real files (directories have no
    // extension to edit). Views must still call edit() explicitly (see
    // FileListView click-to-rename / MainWindow::renameCurrent): NoEditTriggers
    // is set globally so a stray keystroke never starts a rename by accident.
    if (!isParentEntry(index.row())) {
        // A synthetic row names a *place* -- a drive, a saved server, a
        // discovered host -- and none of those names is the user's to change
        // through a file manager's rename. The whole listing is read-only.
        if (m_virtualListing) {
            // nothing editable
        } else if (index.column() == NameColumn) {
            f |= Qt::ItemIsEditable;
        } else if (index.column() == ExtColumn && !fileInfoAt(index.row()).isDir()) {
            f |= Qt::ItemIsEditable;
        }
    }

    // Without ItemIsDragEnabled, QAbstractItemView refuses to start a drag
    // at all -- it never even calls the view's startDrag() override. ".."
    // isn't a real draggable entry, so leave it out.
    if (!isParentEntry(index.row()))
        f |= Qt::ItemIsDragEnabled;
    f |= Qt::ItemIsDropEnabled;

    return f;
}

bool FileSystemModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (role != Qt::EditRole || isParentEntry(index.row()))
        return false;
    const int col = index.column();
    if (col != NameColumn && col != ExtColumn)
        return false;

    const FileInfo info = fileInfoAt(index.row());
    if (!info.isValid())
        return false;

    // flags() already withholds ItemIsEditable across a synthetic listing, so no
    // editor can commit here; refused again rather than relying on that, since
    // setData() is public and a caller could reach it directly.
    if (m_virtualListing)
        return false;

    // The Name column now shows the base name and the Ext column the suffix, so
    // both edits just recombine the two halves into a full file name.
    QString newName;
    if (col == NameColumn) {
        const QString newBase = value.toString().trimmed();
        if (newBase.isEmpty())
            return false; // an empty base name would turn "photo.jpg" into ".jpg"
        // A directory (or an extension-less file) has no suffix to re-append.
        newName = info.suffix().isEmpty() ? newBase
                                          : newBase + QLatin1Char('.') + info.suffix();
    } else { // ExtColumn
        if (info.isDir())
            return false;
        const QString newExt = value.toString().trimmed();
        newName = newExt.isEmpty() ? info.baseName()
                                   : info.baseName() + QLatin1Char('.') + newExt;
    }

    if (newName.isEmpty() || newName == info.name())
        return false;

    const QString oldPath = info.path();

    // Remote tab: never block the GUI thread on a network rename (SFTP alone is a
    // stat + rename round trip, and it contends with the interactive session
    // lock). Hand it to the transfer pool and accept the edit; the panel reloads
    // on completion and the old name simply stays if the rename fails.
    if (m_session) {
        // Reproduce the providers' own sibling-target computation (they all do
        // cleanPath(parentDir + "/" + newName)) so `renamed` records a correct
        // undo entry and keeps the cursor on the renamed row after the reload,
        // exactly as the synchronous path does.
        const QString parent = m_provider->parentPath(m_provider->cleanPath(oldPath));
        const QString parentDir = parent.isEmpty() ? QStringLiteral("/") : parent;
        const QString newPath = m_provider->cleanPath(parentDir + QLatin1Char('/') + newName);
        emit remoteRenameRequested(oldPath, newName);
        emit renamed(oldPath, newPath);
        return true;
    }

    QString newPath;
    switch (m_provider->rename(oldPath, newName, &newPath)) {
    case FileProvider::RenameResult::AlreadyExists:
        emit renameFailed(tr("%1 already exists").arg(newName));
        return false;
    case FileProvider::RenameResult::Failed:
    // Unsupported belongs to moveTo(); every backend implements rename(), so it
    // never reaches here. Treated as a plain failure rather than left out, so
    // the switch stays exhaustive.
    case FileProvider::RenameResult::Unsupported:
        emit renameFailed(tr("Failed to rename %1").arg(info.name()));
        return false;
    case FileProvider::RenameResult::Ok:
        break;
    }

    emit renamed(oldPath, newPath);
    setRootPath(m_rootPath); // reload to pick up the new name/sort position
    return true;
}
