#include "SmbProvider.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QUrl>

#include <libsmbclient.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Bounded TCP reachability probe to `host`:`port`. libsmbclient's own
// smbc_setTimeout does NOT bound the initial TCP connect, so an unreachable /
// black-holed IP (SYN never answered) hangs the worker for the OS default
// (~2 min) instead of the configured timeout -- the user sees "connecting"
// forever and never a failure. A non-blocking connect + select bounded by
// `timeoutMs` fails fast so the session can move on to reconnect/failed. Returns
// true if the port accepts a connection within the budget. Runs on the session
// worker thread.
bool tcpReachable(const QString &host, int port, int timeoutMs) {
    // Accept "[::1]"-style bracketed IPv6 by stripping the brackets for getaddrinfo.
    QString h = host;
    if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']')))
        h = h.mid(1, h.size() - 2);
    const QByteArray hostUtf8 = h.toUtf8();
    const QByteArray portStr = QByteArray::number(port);

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(hostUtf8.constData(), portStr.constData(), &hints, &res) != 0 || !res)
        return false; // name doesn't resolve -> not reachable

    bool reachable = false;
    for (struct addrinfo *ai = res; ai && !reachable; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            reachable = true; // connected immediately (localhost)
        } else if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            if (::select(fd + 1, nullptr, &wset, nullptr, &tv) > 0 && FD_ISSET(fd, &wset)) {
                int soErr = 0;
                socklen_t len = sizeof(soErr);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0)
                    reachable = true;
            }
        }
        ::close(fd);
    }
    freeaddrinfo(res);
    return reachable;
}

// Wraps a libsmbclient file handle so it can travel through the opaque
// FileHandle interface. Opened by openRead/openWrite, closed by closeHandle.
// When `conn` is non-null the handle owns a pooled context borrowed for the
// transfer and all I/O runs on it lock-free; when `conn` is null the handle uses
// the provider's shared interactive context under m_mutex (the fallback path).
// A handle backed by a helper subprocess instead of an in-process context, so
// several reads can be in flight at once. `channel` is borrowed from
// SmbProvider::m_helpers and returned by closeHandle.
struct SmbHelperHandle : FileHandle {
    SmbHelperClient::Channel *channel = nullptr;
    explicit SmbHelperHandle(SmbHelperClient::Channel *c) : channel(c) {}
};

struct SmbHandle : FileHandle {
    SMBCFILE *file = nullptr;
    SMBCCTX *conn = nullptr; // borrowed from the pool; null => shared-context fallback
    bool broken = false;     // an I/O error occurred -> discard the ctx, don't reuse
    explicit SmbHandle(SMBCFILE *f) : file(f) {}
    SmbHandle(SMBCFILE *f, SMBCCTX *c) : file(f), conn(c) {}
};

// Serialises smbc_new_context()/smbc_free_context() across the whole process.
//
// Creating or freeing a context reaches into state libsmbclient keeps globally
// rather than per-context (the loaded smb.conf in libsmbconf, and the talloc
// pools under libcli). Doing either from two threads at once corrupts that
// state and crashes inside the library. The connection pool deliberately runs
// its factory and destroyer without holding the pool lock -- so that a slow
// dial or teardown doesn't stall other borrowers -- and thumbnailing a share
// full of videos is the first thing that reliably drives two workers to
// build/drop contexts at the same moment.
//
// Scoped to the library's own state, so it is process-wide rather than per
// provider, and held only across create/destroy: ordinary reads and writes on
// an already-built context stay fully parallel.
QMutex &smbContextLifecycleMutex() {
    static QMutex mutex;
    return mutex;
}

// libsmbclient's auth callback. It runs synchronously inside a libsmbclient
// call (which already holds the provider's mutex), so it may read the
// provider's credential members directly -- they are set once before any call
// and never mutated afterwards. The SmbProvider* is stashed as the context's
// user data at connect time.
void authCallback(SMBCCTX *ctx, const char * /*srv*/, const char * /*shr*/,
                  char *wg, int wglen, char *un, int unlen, char *pw, int pwlen) {
    auto *self = static_cast<SmbProvider *>(smbc_getOptionUserData(ctx));
    if (!self)
        return;

    auto copyField = [](char *dst, int cap, const QByteArray &src) {
        if (!dst || cap <= 0)
            return;
        const int n = qMin(src.size(), cap - 1);
        std::memcpy(dst, src.constData(), static_cast<size_t>(n));
        dst[n] = '\0';
    };

    // Reaching back into the provider is safe: authCallback only runs while a
    // libsmbclient call the provider itself issued is on the stack.
    copyField(wg, wglen, self->workgroupForAuth().toUtf8());
    copyField(un, unlen, self->userForAuth().toUtf8());
    copyField(pw, pwlen, self->passwordForAuth().toUtf8());
}

} // namespace

SmbProvider::SmbProvider() = default;

SmbProvider::~SmbProvider() {
    // Non-blocking teardown of the transfer pool first: the transfer workers are
    // already joined by the OperationQueue destructor by the time a provider is
    // released, so the pool holds only idle contexts, handed off to a detached
    // reaper so this destructor never blocks on a slow disconnect.
    m_pool.shutdownAsync();
    // Helpers go before disconnect() so no subprocess outlives the provider
    // that spawned it. Their teardown is fast (close a socket, reap a child
    // that exits on EOF), so this does not block the way an SMB disconnect can.
    m_helpers.shutdown();
    disconnect();
}

SMBCCTX *SmbProvider::buildContext(QString *error, bool *authFailed) {
    SMBCCTX *ctx = nullptr;
    {
        // Only the allocation itself is serialised. Everything below either
        // touches this context alone or talks to the network, and holding the
        // process-wide lock across a dial would make every connection wait out
        // every other one's timeout.
        QMutexLocker lifecycleLocker(&smbContextLifecycleMutex());
        ctx = smbc_new_context();
    }
    if (!ctx) {
        if (error)
            *error = QStringLiteral("Failed to allocate SMB context");
        return nullptr;
    }

    smbc_setOptionUserData(ctx, this);
    smbc_setFunctionAuthDataWithContext(ctx, authCallback);
    // This backend targets username/password (and guest) access to Samba/NAS
    // shares, so Kerberos is not required; keep it off and rely on the auth
    // callback.
    smbc_setOptionUseKerberos(ctx, 0);
    smbc_setOptionFallbackAfterKerberos(ctx, 1);
    // Allow an anonymous/guest attempt when no credentials are supplied.
    smbc_setOptionNoAutoAnonymousLogin(ctx, 0);
    smbc_setDebug(ctx, 0);
    // Bound waits on connection establishment and response data (milliseconds).
    smbc_setTimeout(ctx, m_timeoutMs);

    // Initialisation parses smb.conf into the same process-wide state the
    // allocation above touches, so it takes the lock too -- but on its own, not
    // held across the network probe that follows.
    bool initialised = false;
    {
        QMutexLocker lifecycleLocker(&smbContextLifecycleMutex());
        initialised = smbc_init_context(ctx) != nullptr;
    }
    if (!initialised) {
        const int initErrno = errno;
        if (error)
            *error = QStringLiteral("Failed to initialise SMB context: %1")
                         .arg(QString::fromUtf8(std::strerror(initErrno)));
        QMutexLocker lifecycleLocker(&smbContextLifecycleMutex());
        smbc_free_context(ctx, 1);
        return nullptr;
    }

    // Probe: list the server's shares. This forces the actual connection +
    // authentication so a bad host/credential fails here rather than on the
    // first navigation.
    const QByteArray rootUrl = urlFor(QStringLiteral("/")).toUtf8();
    smbc_opendir_fn opendirFn = smbc_getFunctionOpendir(ctx);
    smbc_closedir_fn closedirFn = smbc_getFunctionClosedir(ctx);
    SMBCFILE *dir = opendirFn(ctx, rootUrl.constData());
    if (!dir) {
        const int e = errno;
        // EACCES/EPERM here mean the server rejected the (anonymous) login --
        // credentials are needed. Flag it so the caller prompts for a
        // username/password instead of retrying the same anonymous dial.
        const bool isAuth = (e == EACCES || e == EPERM);
        if (authFailed)
            *authFailed = isAuth;
        if (error) {
            if (isAuth)
                *error = QStringLiteral("Authentication required for \\\\%1: %2")
                             .arg(m_host, QString::fromUtf8(std::strerror(e)));
            else
                *error = QStringLiteral("Cannot open \\\\%1: %2")
                             .arg(m_host, QString::fromUtf8(std::strerror(e)));
        }
        QMutexLocker lifecycleLocker(&smbContextLifecycleMutex());
        smbc_free_context(ctx, 1);
        return nullptr;
    }
    closedirFn(ctx, dir);
    return ctx;
}

void SmbProvider::configurePool() {
    // The Factory builds an independent context off any lock. It captures `this`
    // (only invoked by borrow() during an active transfer, i.e. while the
    // provider is alive); buildContext reads the effectively-const post-connect
    // credentials via the auth callback and m_host. The Destroyer captures
    // nothing, so it is safe on the detached reaper thread.
    m_pool.configure(
        [this](QString *err) -> SMBCCTX * { return buildContext(err); },
        [](SMBCCTX *c) {
            if (c) {
                // Same global state the factory guards; the reaper thread frees
                // contexts while other threads may be building them.
                QMutexLocker lifecycleLocker(&smbContextLifecycleMutex());
                smbc_free_context(c, 1);
            }
        },
        m_maxChannels);
}

void SmbProvider::setMaxTransferChannels(int channels) {
    // Deliberately ignores anything above one: libsmbclient cannot be driven
    // from two threads at once, not even through separate contexts (see the
    // m_maxChannels comment in the header). The setter is kept so callers that
    // tune channel counts generically don't need to special-case SMB.
    Q_UNUSED(channels);
    m_maxChannels = 1;
    m_pool.setMaxSize(m_maxChannels);
}

bool SmbProvider::connectToHost(const QString &host, const QString &user,
                                const QString &password, const QString &workgroup,
                                bool anonymous, QString *error) {
    QMutexLocker locker(&m_mutex);

    if (m_ctx) {
        if (error)
            *error = QStringLiteral("Already connected");
        return false;
    }
    m_lastConnectAuthFailed = false; // reset before any early-return below

    // Validate the host before it is ever concatenated into an smb:// URL.
    // libsmbclient parses the authority as [domain;][user[:pass]@]server[:port],
    // so a host like "real-server@attacker.example" would connect to the
    // attacker while the auth callback still hands over the password -- a
    // credential-leak vector reachable via a tampered bookmarks file. Reject
    // anything that isn't a bare host / IPv4 / [IPv6]; alnum, '.', '-', '_',
    // ':' and brackets are the only authority characters we allow.
    if (host.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty host name");
        return false;
    }
    for (const QChar c : host) {
        const bool ok = c.isLetterOrNumber() || c == QLatin1Char('.') ||
                        c == QLatin1Char('-') || c == QLatin1Char('_') ||
                        c == QLatin1Char(':') || c == QLatin1Char('[') ||
                        c == QLatin1Char(']');
        if (!ok) {
            if (error)
                *error = QStringLiteral("Invalid character in host name: %1").arg(c);
            return false;
        }
    }

    // Stash credentials before init so the auth callback can read them.
    m_host = host;
    m_workgroup = workgroup;
    m_anonymous = anonymous;
    if (anonymous) {
        m_user.clear();
        m_password.clear();
    } else {
        m_user = user;
        m_password = password;
    }

    // Fail fast if the host's SMB port isn't reachable within the timeout, so an
    // unreachable/black-holed IP surfaces "connection failed" in bounded time
    // instead of hanging libsmbclient's un-timed TCP connect for minutes.
    // Reachability only needs a short budget (a live LAN host answers in <100ms);
    // cap it well under the SMB op timeout so a dead host fails fast.
    if (!tcpReachable(host, 445, qMin(m_timeoutMs, 5000))) {
        if (error)
            *error = QStringLiteral("Cannot reach \\\\%1: host unreachable or SMB port closed")
                         .arg(host);
        m_host.clear();
        return false;
    }

    SMBCCTX *ctx = buildContext(error, &m_lastConnectAuthFailed);
    if (!ctx) {
        m_host.clear();
        return false;
    }

    m_ctx = ctx;
    // Publish the label only now: displayName() used to derive it from m_host,
    // which is only non-empty on a live connection, and callers rely on an empty
    // string meaning "not connected yet".
    publishDisplayName();
    // Wire the transfer pool now that credentials are known. The pool builds its
    // own independent contexts lazily on first borrow().
    configurePool();
    // Same for the helper pool: it spawns nothing until the first read asks for
    // a channel, so an ordinary browse session costs no subprocesses at all.
    m_helpers.configure(m_host, m_user, m_password, m_workgroup, m_anonymous, m_timeoutMs);
    return true;
}

void SmbProvider::publishDisplayName() {
    // Called with m_mutex held (connect/disconnect); takes the identity mutex
    // second, per the lock order documented in the header.
    QMutexLocker locker(&m_identityMutex);
    m_displayName = m_host.isEmpty()
                        ? QString()
                        : (m_user.isEmpty() ? m_host : m_user + QLatin1Char('@') + m_host);
}

bool SmbProvider::reconnect(QString *error) {
    // Snapshot credentials before disconnect() clears them. connectToHost() and
    // disconnect() each take m_mutex, so reconnect() must not hold the
    // (non-recursive) lock while calling them.
    QString host, user, password, workgroup;
    bool anonymous;
    {
        QMutexLocker locker(&m_mutex);
        host = m_host;
        user = m_user;
        password = m_password;
        workgroup = m_workgroup;
        anonymous = m_anonymous;
    }
    disconnect();
    return connectToHost(host, user, password, workgroup, anonymous, error);
}

void SmbProvider::disconnect() {
    // Before the lock: the helpers hold credentials that are about to be
    // cleared, and shutdown() takes its own mutex plus waits on child processes.
    m_helpers.shutdown();

    QMutexLocker locker(&m_mutex);
    if (m_ctx) {
        // shutdown_ctx=1 closes any open files/dirs and frees the context.
        // Serialised against context creation elsewhere in the process: pooled
        // transfers may still be dialling on other threads when a tab is closed.
        QMutexLocker lifecycleLocker(&smbContextLifecycleMutex());
        smbc_free_context(m_ctx, 1);
        m_ctx = nullptr;
    }
    m_host.clear();
    // Drop credentials once the context is gone -- no longer needed, and they
    // shouldn't linger in memory for the object's remaining lifetime.
    m_user.clear();
    m_password.clear();
    m_workgroup.clear();
    m_anonymous = false;
    publishDisplayName(); // back to empty: the tab label must stop claiming a link
}

bool SmbProvider::isConnected() const {
    QMutexLocker locker(&m_mutex);
    return m_ctx != nullptr;
}

QString SmbProvider::host() const {
    QMutexLocker locker(&m_mutex);
    return m_host;
}

QString SmbProvider::displayName() const {
    // Reads the snapshot under its own tiny mutex, never m_mutex: the GUI thread
    // calls this to label a tab, and m_mutex can be held for seconds by a large
    // directory listing on the session thread.
    QMutexLocker locker(&m_identityMutex);
    return m_displayName;
}

QString SmbProvider::userForAuth() const { return m_user; }
QString SmbProvider::passwordForAuth() const { return m_password; }
QString SmbProvider::workgroupForAuth() const { return m_workgroup; }

QString SmbProvider::urlFor(const QString &path) const {
    // m_host is set at connect and read here; callers already hold the lock or
    // run before m_ctx is published. Build "smb://host" + POSIX path.
    const QString clean = cleanPath(path);
    QString url = QStringLiteral("smb://") + m_host;
    if (clean == QStringLiteral("/"))
        return url + QLatin1Char('/');

    // Percent-encode every path segment. libsmbclient url-DECODES the path we
    // hand it, so any character a (possibly hostile) server put in a name that
    // is meaningful in an smb:// URL -- '%', '?', '#', space, an encoded '/',
    // non-ASCII bytes -- must be encoded here, otherwise it could redirect the
    // request to a different path/share or inject connection options (e.g.
    // "evil?encrypt=off"). Our own POSIX separators stay literal '/'.
    const QStringList segments = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &seg : segments) {
        url += QLatin1Char('/');
        url += QString::fromLatin1(QUrl::toPercentEncoding(seg));
    }
    return url;
}

namespace {
// Translates a POSIX mode into Qt's permission flags (the owner/group/other
// bits libsmbclient synthesises from the DOS attributes).
QFile::Permissions permsFromMode(mode_t m) {
    QFile::Permissions perms;
    if (m & 0400) perms |= QFile::ReadOwner;
    if (m & 0200) perms |= QFile::WriteOwner;
    if (m & 0100) perms |= QFile::ExeOwner;
    if (m & 0040) perms |= QFile::ReadGroup;
    if (m & 0020) perms |= QFile::WriteGroup;
    if (m & 0010) perms |= QFile::ExeGroup;
    if (m & 0004) perms |= QFile::ReadOther;
    if (m & 0002) perms |= QFile::WriteOther;
    if (m & 0001) perms |= QFile::ExeOther;
    return perms;
}
} // namespace

QVector<FileInfo> SmbProvider::listPlus(const QString &dirPath, const QByteArray &url,
                                        bool showHidden, bool *supported) const {
    // readdirplus2 returns each entry's stat alongside its name, so a directory
    // costs ONE server round-trip instead of one readdir plus one stat per entry.
    // On a 1410-entry share that is the difference between ~60ms and ~5s.
    QVector<FileInfo> result;
    *supported = false;

    smbc_readdirplus2_fn readdirPlus2Fn = smbc_getFunctionReaddirPlus2(m_ctx);
    if (!readdirPlus2Fn)
        return result;

    smbc_opendir_fn opendirFn = smbc_getFunctionOpendir(m_ctx);
    smbc_closedir_fn closedirFn = smbc_getFunctionClosedir(m_ctx);

    SMBCFILE *dir = opendirFn(m_ctx, url.constData());
    if (!dir)
        return result;

    int seen = 0;
    const struct libsmb_file_info *fi = nullptr;
    struct stat st;
    while ((fi = readdirPlus2Fn(m_ctx, dir, &st)) != nullptr) {
        ++seen;
        const QString name = QString::fromUtf8(fi->name);
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;

        // Inside a share every entry is a real file or directory, so the
        // share/server special-casing of the readdir path does not apply here:
        // only dot-prefixed names count as hidden.
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            continue;

        const QString fullPath = cleanPath(dirPath + QLatin1Char('/') + name);
        const bool entryIsDir = S_ISDIR(st.st_mode);
        result.append(FileInfo::fromFields(
            fullPath, name, entryIsDir ? 0 : static_cast<qint64>(st.st_size),
            QDateTime::fromSecsSinceEpoch(static_cast<qint64>(st.st_mtime)), entryIsDir,
            permsFromMode(st.st_mode)));
    }
    closedirFn(m_ctx, dir);

    // A directory always yields at least "." and ".."; zero raw entries means
    // the server/library did not actually serve readdirplus2 here (it returns
    // nothing at the server root, where only readdir enumerates the shares), so
    // the caller must fall back rather than report an empty directory.
    *supported = seen > 0;
    return result;
}

QVector<FileInfo> SmbProvider::list(const QString &path, bool showHidden) const {
    QVector<FileInfo> result;
    const QString dirPath = cleanPath(path);

    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return result;

    const QByteArray url = urlFor(dirPath).toUtf8();

    // Fast path first: one round-trip for the whole directory. It does not work
    // at the server root (share enumeration), which falls through below.
    if (dirPath != QStringLiteral("/")) {
        bool supported = false;
        QVector<FileInfo> plus = listPlus(dirPath, url, showHidden, &supported);
        if (supported)
            return plus;
    }

    smbc_opendir_fn opendirFn = smbc_getFunctionOpendir(m_ctx);
    smbc_readdir_fn readdirFn = smbc_getFunctionReaddir(m_ctx);
    smbc_closedir_fn closedirFn = smbc_getFunctionClosedir(m_ctx);
    smbc_stat_fn statFn = smbc_getFunctionStat(m_ctx);

    SMBCFILE *dir = opendirFn(m_ctx, url.constData());
    if (!dir)
        return result;

    struct smbc_dirent *de = nullptr;
    while ((de = readdirFn(m_ctx, dir)) != nullptr) {
        const QString name = QString::fromUtf8(de->name);
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;

        // Always skip the IPC$ pseudo-share.
        if (de->smbc_type == SMBC_IPC_SHARE)
            continue;

        const bool isShareOrServer =
            de->smbc_type == SMBC_FILE_SHARE || de->smbc_type == SMBC_PRINTER_SHARE ||
            de->smbc_type == SMBC_COMMS_SHARE || de->smbc_type == SMBC_SERVER ||
            de->smbc_type == SMBC_WORKGROUP;

        // Dot-prefixed names are hidden everywhere; a trailing '$' marks an
        // administrative share and is only treated as hidden at the share level
        // -- a regular file/dir named e.g. "foo$" inside a share must stay
        // visible. Both are shown when the user asks for hidden entries.
        const bool hidden = name.startsWith(QLatin1Char('.')) ||
                            (isShareOrServer && name.endsWith(QLatin1Char('$')));
        if (!showHidden && hidden)
            continue;

        const QString fullPath = cleanPath(dirPath + QLatin1Char('/') + name);

        qint64 size = 0;
        QDateTime modified;
        bool entryIsDir = isShareOrServer || de->smbc_type == SMBC_DIR;
        QFile::Permissions perms;

        // Shares/servers don't stat cheaply; only stat real files/dirs so a
        // large listing stays responsive on the parts that carry metadata.
        if (!isShareOrServer) {
            struct stat st;
            const QByteArray entryUrl = urlFor(fullPath).toUtf8();
            if (statFn(m_ctx, entryUrl.constData(), &st) == 0) {
                entryIsDir = S_ISDIR(st.st_mode);
                size = entryIsDir ? 0 : static_cast<qint64>(st.st_size);
                modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(st.st_mtime));
                perms = permsFromMode(st.st_mode);
            }
        }

        result.append(FileInfo::fromFields(fullPath, name, size, modified, entryIsDir, perms));
    }

    closedirFn(m_ctx, dir);
    return result;
}

bool SmbProvider::isDir(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return true; // the server root (share list) behaves as a directory

    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return false;
    smbc_stat_fn statFn = smbc_getFunctionStat(m_ctx);
    struct stat st;
    const QByteArray url = urlFor(clean).toUtf8();
    if (statFn(m_ctx, url.constData(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

bool SmbProvider::exists(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return true;
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return false;
    smbc_stat_fn statFn = smbc_getFunctionStat(m_ctx);
    struct stat st;
    const QByteArray url = urlFor(clean).toUtf8();
    return statFn(m_ctx, url.constData(), &st) == 0;
}

FileProvider::RenameResult SmbProvider::rename(const QString &path, const QString &newName,
                                               QString *newPath) {
    const QString oldPath = cleanPath(path);
    const QString parent = parentPath(oldPath);
    const QString parentDir = parent.isEmpty() ? QStringLiteral("/") : parent;
    const QString destPath = cleanPath(parentDir + QLatin1Char('/') + newName);

    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return RenameResult::Failed;

    smbc_stat_fn statFn = smbc_getFunctionStat(m_ctx);
    smbc_rename_fn renameFn = smbc_getFunctionRename(m_ctx);

    // Reject if something already occupies the destination.
    struct stat st;
    const QByteArray destUrl = urlFor(destPath).toUtf8();
    if (statFn(m_ctx, destUrl.constData(), &st) == 0)
        return RenameResult::AlreadyExists;

    const QByteArray oldUrl = urlFor(oldPath).toUtf8();
    if (renameFn(m_ctx, oldUrl.constData(), m_ctx, destUrl.constData()) != 0)
        return RenameResult::Failed;

    if (newPath)
        *newPath = destPath;
    return RenameResult::Ok;
}

QString SmbProvider::cleanPath(const QString &path) const {
    // POSIX normalisation, independent of the local platform (never QDir).
    // Collapses redundant slashes and resolves "." / ".." segments; always
    // rooted at '/'. Mirrors SftpProvider::cleanPath.
    const QStringList rawParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList stack;
    for (const QString &part : rawParts) {
        if (part == QStringLiteral(".")) {
            continue;
        } else if (part == QStringLiteral("..")) {
            if (!stack.isEmpty())
                stack.removeLast();
        } else {
            stack.append(part);
        }
    }
    if (stack.isEmpty())
        return QStringLiteral("/");
    return QLatin1Char('/') + stack.join(QLatin1Char('/'));
}

QString SmbProvider::parentPath(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return QString();
    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QStringLiteral("/");
    return clean.left(slash);
}

FileHandle *SmbProvider::openRead(const QString &path) {
    const QString clean = cleanPath(path);
    const QByteArray url = urlFor(clean).toUtf8();

    // First choice: a helper subprocess. The in-process pool below is capped at
    // one channel because libsmbclient's global state cannot survive concurrent
    // use, so out-of-process is the only way two reads overlap. Every failure
    // here (no helper installed, pool at capacity, spawn or open failed) simply
    // falls through to the in-process path, which behaves exactly as before.
    if (SmbHelperClient::Channel *channel = m_helpers.acquire()) {
        if (channel->open(url))
            return new SmbHelperHandle(channel);
        m_helpers.release(channel); // open failed; the connection may still be fine
    }

    // Next: borrow an independent context so the transfer runs lock-free and in
    // parallel with other transfers and the interactive context.
    QString perr;
    if (SMBCCTX *conn = m_pool.borrow(&perr)) {
        smbc_open_fn openFn = smbc_getFunctionOpen(conn);
        SMBCFILE *f = openFn(conn, url.constData(), O_RDONLY, 0);
        if (!f) {
            m_pool.release(conn); // context is fine; open just failed (e.g. no file)
            return nullptr;
        }
        return new SmbHandle(f, conn);
    }

    // Fallback: shared interactive context under m_mutex.
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return nullptr;
    smbc_open_fn openFn = smbc_getFunctionOpen(m_ctx);
    SMBCFILE *f = openFn(m_ctx, url.constData(), O_RDONLY, 0);
    if (!f)
        return nullptr;
    return new SmbHandle(f);
}

FileHandle *SmbProvider::openWrite(const QString &path, bool truncate) {
    const QString clean = cleanPath(path);
    const QByteArray url = urlFor(clean).toUtf8();
    // truncate=false (resume) keeps existing bytes so the caller can seek to the
    // append point; O_TRUNC is only added for a fresh/overwrite write.
    int flags = O_WRONLY | O_CREAT;
    if (truncate)
        flags |= O_TRUNC;

    // Preferred path: independent pooled context, lock-free I/O.
    QString perr;
    if (SMBCCTX *conn = m_pool.borrow(&perr)) {
        smbc_open_fn openFn = smbc_getFunctionOpen(conn);
        SMBCFILE *f = openFn(conn, url.constData(), flags, 0644);
        if (!f) {
            m_pool.release(conn);
            return nullptr;
        }
        return new SmbHandle(f, conn);
    }

    // Fallback: shared interactive context under m_mutex.
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return nullptr;
    smbc_open_fn openFn = smbc_getFunctionOpen(m_ctx);
    SMBCFILE *f = openFn(m_ctx, url.constData(), flags, 0644);
    if (!f)
        return nullptr;
    return new SmbHandle(f);
}

qint64 SmbProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    if (auto *hh = dynamic_cast<SmbHelperHandle *>(handle))
        return hh->channel ? hh->channel->read(buffer, maxSize) : -1;
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return -1;
    if (h->conn) {
        // Private context: no shared state, so no lock -- true parallelism.
        smbc_read_fn readFn = smbc_getFunctionRead(h->conn);
        const ssize_t n = readFn(h->conn, h->file, buffer, static_cast<size_t>(maxSize));
        if (n < 0)
            h->broken = true; // physical error -> discard the ctx at close
        return n < 0 ? -1 : static_cast<qint64>(n);
    }
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return -1;
    smbc_read_fn readFn = smbc_getFunctionRead(m_ctx);
    const ssize_t n = readFn(m_ctx, h->file, buffer, static_cast<size_t>(maxSize));
    return n < 0 ? -1 : static_cast<qint64>(n);
}

qint64 SmbProvider::write(FileHandle *handle, const char *buffer, qint64 size) {
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return -1;
    if (h->conn) {
        smbc_write_fn writeFn = smbc_getFunctionWrite(h->conn);
        const ssize_t n = writeFn(h->conn, h->file, buffer, static_cast<size_t>(size));
        if (n < 0)
            h->broken = true;
        return n < 0 ? -1 : static_cast<qint64>(n);
    }
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return -1;
    smbc_write_fn writeFn = smbc_getFunctionWrite(m_ctx);
    const ssize_t n = writeFn(m_ctx, h->file, buffer, static_cast<size_t>(size));
    return n < 0 ? -1 : static_cast<qint64>(n);
}

bool SmbProvider::seek(FileHandle *handle, qint64 offset) {
    if (auto *hh = dynamic_cast<SmbHelperHandle *>(handle))
        return hh->channel && hh->channel->seek(offset);
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return false;
    if (h->conn) {
        smbc_lseek_fn lseekFn = smbc_getFunctionLseek(h->conn);
        return lseekFn(h->conn, h->file, static_cast<off_t>(offset), SEEK_SET) >= 0;
    }
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return false;
    smbc_lseek_fn lseekFn = smbc_getFunctionLseek(m_ctx);
    return lseekFn(m_ctx, h->file, static_cast<off_t>(offset), SEEK_SET) >= 0;
}

qint64 SmbProvider::handleSize(FileHandle *handle) {
    if (auto *hh = dynamic_cast<SmbHelperHandle *>(handle))
        return hh->channel ? hh->channel->size() : -1;
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return -1;
    struct stat st;
    if (h->conn) {
        smbc_fstat_fn fstatFn = smbc_getFunctionFstat(h->conn);
        if (fstatFn(h->conn, h->file, &st) != 0)
            return -1;
        return static_cast<qint64>(st.st_size);
    }
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return -1;
    smbc_fstat_fn fstatFn = smbc_getFunctionFstat(m_ctx);
    if (fstatFn(m_ctx, h->file, &st) != 0)
        return -1;
    return static_cast<qint64>(st.st_size);
}

void SmbProvider::closeHandle(FileHandle *handle) {
    if (auto *hh = dynamic_cast<SmbHelperHandle *>(handle)) {
        if (hh->channel) {
            hh->channel->close();
            // release() destroys the channel if the close revealed a broken
            // helper, so a wedged subprocess is never handed to the next read.
            m_helpers.release(hh->channel);
        }
        delete hh;
        return;
    }
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h)
        return;
    if (h->conn) {
        // Pooled context: close the file on it (lock-free), then return the
        // context to the pool -- or discard it if an I/O error was seen or the
        // close itself fails, so a poisoned context is never reused.
        bool ok = true;
        if (h->file) {
            smbc_close_fn closeFn = smbc_getFunctionClose(h->conn);
            ok = closeFn(h->conn, h->file) == 0;
        }
        if (h->broken || !ok)
            m_pool.discard(h->conn);
        else
            m_pool.release(h->conn);
        delete h;
        return;
    }
    // Fallback handle: shared context under m_mutex.
    {
        QMutexLocker locker(&m_mutex);
        if (m_ctx && h->file) {
            smbc_close_fn closeFn = smbc_getFunctionClose(m_ctx);
            closeFn(m_ctx, h->file);
        }
    }
    delete h;
}

int SmbProvider::maxReadChannels() const {
    // The helper pool answers 1 until a subprocess has actually connected, so a
    // caller never sizes its workers for parallelism that isn't there. It rises
    // to the real cap after the first successful read, and a caller that asks
    // again then gets the better number.
    return m_helpers.provenChannels();
}

bool SmbProvider::remove(const QString &path) {
    const QString clean = cleanPath(path);
    // isDir() locks m_mutex itself, so query type before taking the lock here.
    const bool dir = isDir(clean);

    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return false;
    const QByteArray url = urlFor(clean).toUtf8();
    if (dir) {
        smbc_rmdir_fn rmdirFn = smbc_getFunctionRmdir(m_ctx);
        return rmdirFn(m_ctx, url.constData()) == 0;
    }
    smbc_unlink_fn unlinkFn = smbc_getFunctionUnlink(m_ctx);
    return unlinkFn(m_ctx, url.constData()) == 0;
}

bool SmbProvider::mkdir(const QString &path) {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return false;
    smbc_mkdir_fn mkdirFn = smbc_getFunctionMkdir(m_ctx);
    const QByteArray url = urlFor(clean).toUtf8();
    return mkdirFn(m_ctx, url.constData(), 0755) == 0;
}
