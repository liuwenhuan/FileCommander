#include "SftpProvider.h"

#include <QDateTime>
#include <QMutexLocker>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cerrno>
#include <cstring>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Global one-time libssh2_init(0). libssh2 requires a single init before any
// session is created and a matching exit at teardown; a function-local static
// gives us thread-safe once-only init for the process lifetime.
void ensureLibssh2Init() {
    static const int rc = []() {
        return libssh2_init(0);
    }();
    (void)rc;
}

// Translates the last libssh2 session error into a human-readable string.
QString sessionError(LIBSSH2_SESSION *session, const QString &fallback) {
    if (!session)
        return fallback;
    char *msg = nullptr;
    int len = 0;
    const int code = libssh2_session_last_error(session, &msg, &len, 0);
    if (msg && len > 0)
        return QStringLiteral("%1 (libssh2 error %2)").arg(QString::fromUtf8(msg, len)).arg(code);
    return fallback;
}

// Opens a blocking TCP connection to host:port. Returns the socket fd, or -1
// with *error populated.
int openSocket(const QString &host, int port, QString *error) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray portStr = QByteArray::number(port);

    struct addrinfo *result = nullptr;
    const int gai = ::getaddrinfo(hostUtf8.constData(), portStr.constData(), &hints, &result);
    if (gai != 0) {
        if (error)
            *error = QStringLiteral("Cannot resolve host '%1': %2")
                         .arg(host, QString::fromUtf8(gai_strerror(gai)));
        return -1;
    }

    int sock = -1;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;
        if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
            break; // connected
        ::close(sock);
        sock = -1;
    }
    ::freeaddrinfo(result);

    if (sock < 0 && error)
        *error = QStringLiteral("Cannot connect to %1:%2: %3")
                     .arg(host)
                     .arg(port)
                     .arg(QString::fromUtf8(std::strerror(errno)));
    return sock;
}

} // namespace

SftpProvider::SftpProvider() {
    ensureLibssh2Init();
}

SftpProvider::~SftpProvider() {
    disconnect();
}

bool SftpProvider::connectToHost(const QString &host, int port, const QString &user,
                                 const QString &password, QString *error) {
    QMutexLocker locker(&m_mutex);

    if (m_session) {
        if (error)
            *error = QStringLiteral("Already connected");
        return false;
    }

    const int sock = openSocket(host, port, error);
    if (sock < 0)
        return false;

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        if (error)
            *error = QStringLiteral("Failed to initialise SSH session");
        ::close(sock);
        return false;
    }
    // Blocking mode keeps the flow simple: every libssh2 call returns only once
    // complete rather than yielding LIBSSH2_ERROR_EAGAIN.
    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, sock) != 0) {
        if (error)
            *error = sessionError(session, QStringLiteral("SSH handshake failed"));
        libssh2_session_free(session);
        ::close(sock);
        return false;
    }

    const QByteArray userUtf8 = user.toUtf8();
    bool authed = false;
    // Password auth first when a password was supplied.
    if (!password.isEmpty()) {
        const QByteArray passUtf8 = password.toUtf8();
        authed = libssh2_userauth_password(session, userUtf8.constData(),
                                           passUtf8.constData()) == 0;
    }
    // Fall back to public-key auth from the usual ~/.ssh keys (also lets a
    // password-less connection work when a key is set up).
    if (!authed) {
        const QByteArray home = qgetenv("HOME");
        for (const char *keyName : {"id_ed25519", "id_rsa", "id_ecdsa"}) {
            const QByteArray priv = home + "/.ssh/" + keyName;
            const QByteArray pub = priv + ".pub";
            if (libssh2_userauth_publickey_fromfile(session, userUtf8.constData(),
                                                    pub.constData(), priv.constData(),
                                                    nullptr) == 0) {
                authed = true;
                break;
            }
        }
    }
    if (!authed) {
        if (error)
            *error = sessionError(session, QStringLiteral("Authentication failed"));
        libssh2_session_disconnect(session, "auth failed");
        libssh2_session_free(session);
        ::close(sock);
        return false;
    }

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);
    if (!sftp) {
        if (error)
            *error = sessionError(session, QStringLiteral("Failed to start SFTP subsystem"));
        libssh2_session_disconnect(session, "sftp init failed");
        libssh2_session_free(session);
        ::close(sock);
        return false;
    }

    m_socket = sock;
    m_session = session;
    m_sftp = sftp;
    m_host = host;
    return true;
}

void SftpProvider::disconnect() {
    QMutexLocker locker(&m_mutex);
    if (m_sftp) {
        libssh2_sftp_shutdown(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect(m_session, "bye");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
    m_host.clear();
}

bool SftpProvider::isConnected() const {
    QMutexLocker locker(&m_mutex);
    return m_sftp != nullptr;
}

QString SftpProvider::host() const {
    QMutexLocker locker(&m_mutex);
    return m_host;
}

QVector<FileInfo> SftpProvider::list(const QString &path, bool showHidden) const {
    QVector<FileInfo> result;
    const QString dirPath = cleanPath(path);

    QMutexLocker locker(&m_mutex);
    if (!m_sftp)
        return result;

    const QByteArray pathUtf8 = dirPath.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(m_sftp, pathUtf8.constData());
    if (!handle)
        return result;

    char nameBuf[512];
    char longEntry[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc = 0;
    while ((rc = libssh2_sftp_readdir_ex(handle, nameBuf, sizeof(nameBuf), longEntry,
                                         sizeof(longEntry), &attrs)) > 0) {
        const QString name = QString::fromUtf8(nameBuf, rc);
        if (name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            continue;

        const bool hasPerms = attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS;
        const bool entryIsDir = hasPerms && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);

        const qint64 size =
            (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? static_cast<qint64>(attrs.filesize) : 0;
        QDateTime modified;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
            modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attrs.mtime));

        // POSIX permission bits from the remote map directly onto QFile's owner/
        // group/other read-write-exec flags (same bit layout, shifted by owner).
        QFile::Permissions perms;
        if (hasPerms) {
            const unsigned long m = attrs.permissions;
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

        const QString fullPath = cleanPath(dirPath + QLatin1Char('/') + name);
        result.append(FileInfo::fromFields(fullPath, name, size, modified, entryIsDir, perms));
    }

    libssh2_sftp_closedir(handle);
    return result;
}

bool SftpProvider::isDir(const QString &path) const {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_sftp)
        return false;
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    const QByteArray pathUtf8 = clean.toUtf8();
    if (libssh2_sftp_stat(m_sftp, pathUtf8.constData(), &attrs) != 0)
        return false;
    if (!(attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
        return false;
    return LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
}

bool SftpProvider::exists(const QString &path) const {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_sftp)
        return false;
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    const QByteArray pathUtf8 = clean.toUtf8();
    return libssh2_sftp_stat(m_sftp, pathUtf8.constData(), &attrs) == 0;
}

FileProvider::RenameResult SftpProvider::rename(const QString &path, const QString &newName,
                                                QString *newPath) {
    const QString oldPath = cleanPath(path);
    const QString parent = parentPath(oldPath);
    // Sibling target: parent + "/" + newName, POSIX-normalised. parentPath()
    // returns "" only for the root itself, in which case fall back to "/".
    const QString parentDir = parent.isEmpty() ? QStringLiteral("/") : parent;
    const QString destPath = cleanPath(parentDir + QLatin1Char('/') + newName);

    QMutexLocker locker(&m_mutex);
    if (!m_sftp)
        return RenameResult::Failed;

    // Reject if something already occupies the destination.
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    const QByteArray destUtf8 = destPath.toUtf8();
    if (libssh2_sftp_stat(m_sftp, destUtf8.constData(), &attrs) == 0)
        return RenameResult::AlreadyExists;

    const QByteArray oldUtf8 = oldPath.toUtf8();
    if (libssh2_sftp_rename(m_sftp, oldUtf8.constData(), destUtf8.constData()) != 0)
        return RenameResult::Failed;

    if (newPath)
        *newPath = destPath;
    return RenameResult::Ok;
}

QString SftpProvider::cleanPath(const QString &path) const {
    // POSIX normalisation, independent of the local platform (never QDir, which
    // carries local-filesystem semantics). Collapses redundant slashes and
    // resolves "." / ".." segments; always rooted at '/'.
    const QStringList rawParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList stack;
    for (const QString &part : rawParts) {
        if (part == QStringLiteral(".")) {
            continue;
        } else if (part == QStringLiteral("..")) {
            if (!stack.isEmpty())
                stack.removeLast();
            // ".." at root stays at root (can't go above '/').
        } else {
            stack.append(part);
        }
    }
    if (stack.isEmpty())
        return QStringLiteral("/");
    return QLatin1Char('/') + stack.join(QLatin1Char('/'));
}

QString SftpProvider::parentPath(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return QString(); // already at the root
    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QStringLiteral("/");
    return clean.left(slash);
}
