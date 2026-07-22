#include "SmbProvider.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QUrl>

#include <libsmbclient.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>

namespace {

// Wraps a libsmbclient file handle so it can travel through the opaque
// FileHandle interface. Opened by openRead/openWrite, closed by closeHandle.
struct SmbHandle : FileHandle {
    SMBCFILE *file = nullptr;
    explicit SmbHandle(SMBCFILE *f) : file(f) {}
};

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
    disconnect();
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

    // Stash credentials before init so the auth callback can read them.
    m_host = host;
    m_workgroup = workgroup;
    if (anonymous) {
        m_user.clear();
        m_password.clear();
    } else {
        m_user = user;
        m_password = password;
    }

    SMBCCTX *ctx = smbc_new_context();
    if (!ctx) {
        if (error)
            *error = QStringLiteral("Failed to allocate SMB context");
        m_host.clear();
        return false;
    }

    smbc_setOptionUserData(ctx, this);
    smbc_setFunctionAuthDataWithContext(ctx, authCallback);
    // This backend targets username/password (and guest) access to Samba/NAS
    // shares, so Kerberos is not required; keep it off and rely on the auth
    // callback. (An AD/domain deployment that wants Kerberos mutual auth would
    // need a per-connection toggle -- deferred.)
    smbc_setOptionUseKerberos(ctx, 0);
    smbc_setOptionFallbackAfterKerberos(ctx, 1);
    // Allow an anonymous/guest attempt when no credentials are supplied.
    smbc_setOptionNoAutoAnonymousLogin(ctx, 0);
    smbc_setDebug(ctx, 0);

    if (!smbc_init_context(ctx)) {
        if (error)
            *error = QStringLiteral("Failed to initialise SMB context: %1")
                         .arg(QString::fromUtf8(std::strerror(errno)));
        smbc_free_context(ctx, 1);
        m_host.clear();
        return false;
    }

    m_ctx = ctx;

    // Probe: list the server's shares. This forces the actual connection +
    // authentication so a bad host/credential fails here rather than on the
    // first navigation.
    const QByteArray rootUrl = urlFor(QStringLiteral("/")).toUtf8();
    smbc_opendir_fn opendirFn = smbc_getFunctionOpendir(ctx);
    smbc_closedir_fn closedirFn = smbc_getFunctionClosedir(ctx);
    SMBCFILE *dir = opendirFn(ctx, rootUrl.constData());
    if (!dir) {
        if (error)
            *error = QStringLiteral("Cannot open \\\\%1: %2")
                         .arg(host, QString::fromUtf8(std::strerror(errno)));
        smbc_free_context(ctx, 1);
        m_ctx = nullptr;
        m_host.clear();
        return false;
    }
    closedirFn(ctx, dir);
    return true;
}

void SmbProvider::disconnect() {
    QMutexLocker locker(&m_mutex);
    if (m_ctx) {
        // shutdown_ctx=1 closes any open files/dirs and frees the context.
        smbc_free_context(m_ctx, 1);
        m_ctx = nullptr;
    }
    m_host.clear();
    // Drop credentials once the context is gone -- no longer needed, and they
    // shouldn't linger in memory for the object's remaining lifetime.
    m_user.clear();
    m_password.clear();
    m_workgroup.clear();
}

bool SmbProvider::isConnected() const {
    QMutexLocker locker(&m_mutex);
    return m_ctx != nullptr;
}

QString SmbProvider::host() const {
    QMutexLocker locker(&m_mutex);
    return m_host;
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

QVector<FileInfo> SmbProvider::list(const QString &path, bool showHidden) const {
    QVector<FileInfo> result;
    const QString dirPath = cleanPath(path);

    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return result;

    smbc_opendir_fn opendirFn = smbc_getFunctionOpendir(m_ctx);
    smbc_readdir_fn readdirFn = smbc_getFunctionReaddir(m_ctx);
    smbc_closedir_fn closedirFn = smbc_getFunctionClosedir(m_ctx);
    smbc_stat_fn statFn = smbc_getFunctionStat(m_ctx);

    const QByteArray url = urlFor(dirPath).toUtf8();
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

                const mode_t m = st.st_mode;
                if (m & 0400) perms |= QFile::ReadOwner;
                if (m & 0200) perms |= QFile::WriteOwner;
                if (m & 0100) perms |= QFile::ExeOwner;
                if (m & 0040) perms |= QFile::ReadGroup;
                if (m & 0020) perms |= QFile::WriteGroup;
                if (m & 0010) perms |= QFile::ExeGroup;
                if (m & 0004) perms |= QFile::ReadOther;
                if (m & 0002) perms |= QFile::WriteOther;
                if (m & 0001) perms |= QFile::ExeOther;
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
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return nullptr;
    smbc_open_fn openFn = smbc_getFunctionOpen(m_ctx);
    const QByteArray url = urlFor(clean).toUtf8();
    SMBCFILE *f = openFn(m_ctx, url.constData(), O_RDONLY, 0);
    if (!f)
        return nullptr;
    return new SmbHandle(f);
}

FileHandle *SmbProvider::openWrite(const QString &path, bool truncate) {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return nullptr;
    // truncate=false (resume) keeps existing bytes so the caller can seek to the
    // append point; O_TRUNC is only added for a fresh/overwrite write.
    int flags = O_WRONLY | O_CREAT;
    if (truncate)
        flags |= O_TRUNC;
    smbc_open_fn openFn = smbc_getFunctionOpen(m_ctx);
    const QByteArray url = urlFor(clean).toUtf8();
    SMBCFILE *f = openFn(m_ctx, url.constData(), flags, 0644);
    if (!f)
        return nullptr;
    return new SmbHandle(f);
}

qint64 SmbProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return -1;
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
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return -1;
    smbc_write_fn writeFn = smbc_getFunctionWrite(m_ctx);
    const ssize_t n = writeFn(m_ctx, h->file, buffer, static_cast<size_t>(size));
    return n < 0 ? -1 : static_cast<qint64>(n);
}

bool SmbProvider::seek(FileHandle *handle, qint64 offset) {
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return false;
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return false;
    smbc_lseek_fn lseekFn = smbc_getFunctionLseek(m_ctx);
    return lseekFn(m_ctx, h->file, static_cast<off_t>(offset), SEEK_SET) >= 0;
}

qint64 SmbProvider::handleSize(FileHandle *handle) {
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h || !h->file)
        return -1;
    QMutexLocker locker(&m_mutex);
    if (!m_ctx)
        return -1;
    smbc_fstat_fn fstatFn = smbc_getFunctionFstat(m_ctx);
    struct stat st;
    if (fstatFn(m_ctx, h->file, &st) != 0)
        return -1;
    return static_cast<qint64>(st.st_size);
}

void SmbProvider::closeHandle(FileHandle *handle) {
    auto *h = static_cast<SmbHandle *>(handle);
    if (!h)
        return;
    {
        QMutexLocker locker(&m_mutex);
        if (m_ctx && h->file) {
            smbc_close_fn closeFn = smbc_getFunctionClose(m_ctx);
            closeFn(m_ctx, h->file);
        }
    }
    delete h;
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
