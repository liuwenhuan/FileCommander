#include "WindowsSmbProvider.h"
#include "ProviderPath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>

#include <Windows.h>
#include <Lm.h>

namespace {
class WindowsSmbHandle : public FileHandle {
public:
    explicit WindowsSmbHandle(const QString &path) : file(path) {}
    QFile file;
};

QStringList providerSegments(const QString &path, bool *valid = nullptr) {
    bool ok = true;
    const QString normalized = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    for (const QString &part : normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral("..")) {
            ok = false;
            break;
        }
    }
    if (valid)
        *valid = ok;
    if (!ok)
        return {};
    return fc::ProviderPath::normalizeRooted(normalized)
        .split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

bool isValidProviderPath(const QString &path) {
    bool valid = false;
    providerSegments(path, &valid);
    return valid;
}

QFile::Permissions permissionsFor(const QFileInfo &info) {
    return info.permissions();
}

// Whether anything is listening for SMB on `host`, within `timeoutMs`.
//
// This exists because WNetAddConnection2 takes no timeout: reaching a host that
// is simply not there costs the OS default (measured ~22s per attempt here),
// and NetworkSession retries, so the user waits the better part of a minute to
// be told a name does not resolve. A bounded probe answers that in milliseconds.
//
// 445 first (SMB over TCP, everything since Windows 2000), then 139 for the
// NetBIOS session service that older and some NAS boxes still use. Reachable on
// either is enough to hand over to the real connect.
bool smbPortReachable(const QString &host, int timeoutMs) {
    // The budget is split across the two ports rather than spent on each, so the
    // caller's timeout bounds the whole probe. Trying both at the full budget
    // would take twice as long as asked for against a host that answers on
    // neither -- which is precisely the case the timeout is meant to bound.
    const int perPort = qMax(1500, timeoutMs / 2);
    for (quint16 port : {quint16(445), quint16(139)}) {
        QTcpSocket socket;
        socket.connectToHost(host, port);
        if (socket.waitForConnected(perPort)) {
            socket.abort();
            return true;
        }
        // A name that does not resolve fails the same way on both ports, so the
        // second attempt would only burn the rest of the budget for nothing.
        if (socket.error() == QAbstractSocket::HostNotFoundError)
            return false;
    }
    return false;
}
}

QString WindowsSmbProvider::normalizeProviderPath(const QString &path) {
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return fc::ProviderPath::normalizeRooted(normalized);
}

QString WindowsSmbProvider::parentProviderPath(const QString &path) {
    return fc::ProviderPath::parent(normalizeProviderPath(path));
}

QString WindowsSmbProvider::providerPathToUnc(const QString &host, const QString &path,
                                              QString *error) {
    if (error)
        error->clear();
    if (host.isEmpty() || host.contains(QLatin1Char('/')) ||
        host.contains(QLatin1Char('\\')) || host == QStringLiteral(".") ||
        host == QStringLiteral("..")) {
        if (error)
            *error = QStringLiteral("Invalid SMB server name.");
        return {};
    }
    bool valid = false;
    const QStringList parts = providerSegments(path, &valid);
    if (!valid) {
        if (error)
            *error = QStringLiteral("SMB path traversal is not allowed.");
        return {};
    }
    QString unc = QStringLiteral("\\\\") + host;
    if (!parts.isEmpty())
        unc += QLatin1Char('\\') + parts.join(QLatin1Char('\\'));
    return unc;
}

bool WindowsSmbProvider::connectToHost(const QString &host, const QString &user,
                                       const QString &password, const QString &workgroup,
                                       bool anonymous, QString *error) {
    m_lastConnectAuthFailed = false;
    if (providerPathToUnc(host, QStringLiteral("/"), error).isEmpty())
        return false;
    m_host = host;
    m_user = user;
    m_password = password;
    m_workgroup = workgroup;
    m_anonymous = anonymous;
    const QString qualifiedUser =
        workgroup.isEmpty() || user.isEmpty() ? user : workgroup + QLatin1Char('\\') + user;
    m_session.setCredentials(qualifiedUser, password, anonymous);

    // Actually reach the server. This used to return true without touching the
    // network -- it only validated that the host name was a syntactically legal
    // UNC component -- so no credential rejection could ever be observed here,
    // NetworkSession never saw a failed connect, and the login prompt it drives
    // could not fire. A server that refused anonymous access therefore opened as
    // an empty listing with nothing said.
    if (!smbPortReachable(host, m_timeoutMs)) {
        if (error)
            *error = QStringLiteral("Cannot reach SMB on %1.").arg(host);
        return false; // not a credential problem: leave the auth flag clear
    }

    const WindowsSmbSession::Result result = m_session.connectToServer(host, error);
    if (result == WindowsSmbSession::Result::Connected) {
        // Windows refuses a second session to one server under a different user
        // name, so when it hands back the one it already has, the browse runs as
        // whoever opened that -- not as whoever was asked for here. Record it so
        // the panel can say so rather than let the identity change go unnoticed.
        m_reusedSession = m_session.lastConnectBorrowed();
        m_reusedSessionUser =
            m_reusedSession ? WindowsSmbSession::existingSessionUser(host) : QString();
        return true;
    }
    m_reusedSession = false;
    m_reusedSessionUser.clear();
    // The flag NetworkSession reads to decide "prompt" versus "retry": set it
    // from the real status code rather than leaving the session to guess from
    // the message text.
    m_lastConnectAuthFailed = result == WindowsSmbSession::Result::AuthRequired;
    // m_host is deliberately kept: reconnect() refuses to run without it, so
    // clearing it here would make a single failed dial permanently unrecoverable
    // -- including the credentialed retry the login prompt exists to drive.
    // isConnected() asks the session whether a link is actually held, so a
    // remembered host name cannot make this look connected.
    return false;
}

void WindowsSmbProvider::disconnect() {
    m_session.disconnectOwned();
    m_host.clear();
    m_user.clear();
    m_password.clear();
    m_workgroup.clear();
    m_anonymous = false;
    m_reusedSession = false;
    m_reusedSessionUser.clear();
    noteListResult(ListStatus::Ok);
}

void WindowsSmbProvider::noteListResult(ListStatus status, const QString &error) const {
    m_lastListStatus = status;
    m_lastListError = error;
}

QString WindowsSmbProvider::displayName() const {
    return m_user.isEmpty() ? m_host : m_user + QLatin1Char('@') + m_host;
}

RemoteLocation WindowsSmbProvider::remoteLocation() const {
    RemoteLocation result;
    result.scheme = QStringLiteral("smb");
    result.host = m_host;
    result.user = m_user;
    result.password = m_password;
    result.anonymous = m_anonymous;
    return result;
}

QString WindowsSmbProvider::shellAccessiblePath(const QString &path) const {
    QString error;
    return uncFor(path, &error);
}

bool WindowsSmbProvider::reconnect(QString *error) {
    if (m_host.isEmpty()) {
        if (error)
            *error = QStringLiteral("No SMB server has been configured.");
        return false;
    }
    m_session.disconnectOwned();
    return connectToHost(m_host, m_user, m_password, m_workgroup, m_anonymous, error);
}

QString WindowsSmbProvider::uncFor(const QString &path, QString *error) const {
    return providerPathToUnc(m_host, path, error);
}

WindowsSmbSession::Result WindowsSmbProvider::ensureShareFor(const QString &path,
                                                             QString *error) const {
    bool valid = false;
    const QStringList parts = providerSegments(path, &valid);
    if (!valid) {
        if (error)
            *error = QStringLiteral("SMB path traversal is not allowed.");
        return WindowsSmbSession::Result::Failed;
    }
    if (parts.isEmpty())
        return WindowsSmbSession::Result::Connected; // the host root needs no share
    const QString share = QStringLiteral("\\\\") + m_host + QLatin1Char('\\') + parts.first();
    return m_session.ensureConnected(share, error);
}

QVector<FileInfo> WindowsSmbProvider::list(const QString &path, bool showHidden) const {
    QVector<FileInfo> result;
    noteListResult(ListStatus::Ok);
    if (!isValidProviderPath(path)) {
        noteListResult(ListStatus::Failed, QStringLiteral("SMB path traversal is not allowed."));
        return result;
    }
    if (!isConnected()) {
        // Without a session, NetShareEnum still answers -- with ERROR_ACCESS_
        // DENIED, because Windows could not authenticate to a host that is not
        // even reachable. Classified naively that becomes "ask for a password",
        // which is the wrong recovery for a server that is switched off. The
        // session normally never lists before connecting; this makes the wrong
        // answer impossible rather than merely unlikely.
        noteListResult(ListStatus::Failed, QStringLiteral("Not connected to %1.").arg(m_host));
        return result;
    }
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/")) {
        SHARE_INFO_1 *buffer = nullptr;
        DWORD read = 0;
        DWORD total = 0;
        const NET_API_STATUS status = NetShareEnum(
            reinterpret_cast<LPWSTR>(const_cast<ushort *>(m_host.utf16())), 1,
            reinterpret_cast<LPBYTE *>(&buffer), MAX_PREFERRED_LENGTH, &read, &total,
            nullptr);
        if (status != NERR_Success && status != ERROR_MORE_DATA) {
            // The status used to be discarded, so a server that refused to
            // enumerate its shares was reported as a server with no shares.
            // Access denied is reported separately because it is answerable:
            // the session turns it into a login prompt and lists again with
            // whatever the user types.
            const WindowsSmbSession::Result classified = WindowsSmbSession::classify(status);
            noteListResult(classified == WindowsSmbSession::Result::AuthRequired
                               ? ListStatus::AccessDenied
                               : ListStatus::Failed,
                           WindowsSmbSession::describe(status));
            if (buffer)
                NetApiBufferFree(buffer);
            return result;
        }
        for (DWORD i = 0; i < read; ++i) {
            // shi1_type carries flags (STYPE_SPECIAL 0x80000000, STYPE_TEMPORARY
            // 0x40000000) OR'd onto the kind, so it has to be masked before it is
            // compared. Testing the raw value for equality happened to hide the
            // admin shares (C$, ADMIN$ are DISKTREE|SPECIAL) but hid a temporary
            // disk share too, for no reason anyone chose.
            const DWORD kind = buffer[i].shi1_type & STYPE_MASK;
            if (kind != STYPE_DISKTREE)
                continue;
            // Administrative shares stay hidden deliberately: they are every
            // drive root plus the remote-admin pipe, they need admin rights to
            // open, and Explorer does not show them either.
            if (buffer[i].shi1_type & STYPE_SPECIAL)
                continue;
            if (!buffer[i].shi1_netname)
                continue;
            const QString name = QString::fromWCharArray(buffer[i].shi1_netname);
            if (name.isEmpty())
                continue;
            result.append(
                FileInfo::fromFields(QStringLiteral("/") + name, name, 0, {}, true, {}));
        }
        // MAX_PREFERRED_LENGTH asks the API to allocate whatever it needs, so a
        // short read means entries were genuinely dropped rather than paged.
        // Saying so beats presenting a truncated share list as the whole truth.
        if (status == ERROR_MORE_DATA || (total > read && read != total)) {
            noteListResult(ListStatus::Failed,
                           QStringLiteral("Only %1 of %2 shares could be listed.")
                               .arg(read)
                               .arg(total));
        }
        if (buffer)
            NetApiBufferFree(buffer);
        return result;
    }

    QString shareError;
    const WindowsSmbSession::Result share = ensureShareFor(clean, &shareError);
    if (share != WindowsSmbSession::Result::Connected) {
        noteListResult(share == WindowsSmbSession::Result::AuthRequired
                           ? ListStatus::AccessDenied
                           : ListStatus::Failed,
                       shareError);
        return result;
    }

    const QString unc = uncFor(clean);
    QDir dir(unc);
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden)
        filters |= QDir::Hidden;
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::DirsFirst | QDir::Name);
    if (entries.isEmpty() && !dir.exists()) {
        // An unreadable directory and an empty one both come back as an empty
        // list; only exists() separates them. GetLastError still holds the
        // reason the enumeration failed, so the denial can be named.
        const DWORD status = GetLastError();
        const WindowsSmbSession::Result classified = WindowsSmbSession::classify(status);
        noteListResult(classified == WindowsSmbSession::Result::AuthRequired
                           ? ListStatus::AccessDenied
                           : ListStatus::Failed,
                       WindowsSmbSession::describe(status));
        return result;
    }
    for (const QFileInfo &info : entries) {
        const QString child =
            clean == QStringLiteral("/") ? clean + info.fileName()
                                         : clean + QLatin1Char('/') + info.fileName();
        result.append(FileInfo::fromFields(child, info.fileName(), info.size(),
                                           info.lastModified(), info.isDir(),
                                           permissionsFor(info)));
    }
    return result;
}

bool WindowsSmbProvider::isDir(const QString &path) const {
    if (!isValidProviderPath(path))
        return false;
    if (cleanPath(path) == QStringLiteral("/")) {
        // The host root is a directory only while there is a session to the
        // host. Answering an unconditional true made this useless as the
        // liveness probe NetworkSession's heartbeat and drop detection both run
        // through it -- a server that had gone away still reported its root as a
        // perfectly good directory.
        return isConnected();
    }
    return shareReady(path) && QFileInfo(uncFor(path)).isDir();
}

QString WindowsSmbProvider::cleanPath(const QString &path) const {
    return normalizeProviderPath(path);
}

QString WindowsSmbProvider::parentPath(const QString &path) const {
    return parentProviderPath(path);
}

bool WindowsSmbProvider::exists(const QString &path) const {
    if (!isValidProviderPath(path))
        return false;
    if (cleanPath(path) == QStringLiteral("/"))
        return isConnected();
    return shareReady(path) && QFileInfo::exists(uncFor(path));
}

FileProvider::RenameResult WindowsSmbProvider::rename(
    const QString &path, const QString &newName, QString *newPath) {
    const QString oldPath = cleanPath(path);
    const QString destination = fc::ProviderPath::sibling(oldPath, newName);
    if (destination.isEmpty())
        return RenameResult::Failed;
    const RenameResult result = moveTo(path, destination);
    if (result == RenameResult::Ok && newPath)
        *newPath = destination;
    return result;
}

FileProvider::RenameResult WindowsSmbProvider::moveTo(
    const QString &srcPath, const QString &dstPath) {
    bool srcValid = false;
    bool dstValid = false;
    const QStringList src = providerSegments(srcPath, &srcValid);
    const QStringList dst = providerSegments(dstPath, &dstValid);
    if (!srcValid || !dstValid || src.isEmpty() || dst.isEmpty() ||
        src.first().compare(dst.first(), Qt::CaseInsensitive) != 0)
        return RenameResult::Unsupported;
    if (!shareReady(srcPath) || QFileInfo::exists(uncFor(dstPath)))
        return QFileInfo::exists(uncFor(dstPath)) ? RenameResult::AlreadyExists
                                                  : RenameResult::Failed;
    return QDir().rename(uncFor(srcPath), uncFor(dstPath)) ? RenameResult::Ok
                                                           : RenameResult::Failed;
}

FileHandle *WindowsSmbProvider::openRead(const QString &path) {
    if (!shareReady(path))
        return nullptr;
    auto *handle = new WindowsSmbHandle(uncFor(path));
    if (!handle->file.open(QIODevice::ReadOnly)) {
        delete handle;
        return nullptr;
    }
    return handle;
}

FileHandle *WindowsSmbProvider::openWrite(const QString &path, bool truncate) {
    if (!shareReady(path))
        return nullptr;
    auto *handle = new WindowsSmbHandle(uncFor(path));
    const QIODevice::OpenMode mode =
        QIODevice::WriteOnly | (truncate ? QIODevice::Truncate : QIODevice::Append);
    if (!handle->file.open(mode)) {
        delete handle;
        return nullptr;
    }
    return handle;
}

qint64 WindowsSmbProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    auto *smb = static_cast<WindowsSmbHandle *>(handle);
    return smb ? smb->file.read(buffer, maxSize) : -1;
}

qint64 WindowsSmbProvider::write(FileHandle *handle, const char *buffer, qint64 size) {
    auto *smb = static_cast<WindowsSmbHandle *>(handle);
    return smb ? smb->file.write(buffer, size) : -1;
}

bool WindowsSmbProvider::seek(FileHandle *handle, qint64 offset) {
    auto *smb = static_cast<WindowsSmbHandle *>(handle);
    return smb && smb->file.seek(offset);
}

qint64 WindowsSmbProvider::handleSize(FileHandle *handle) {
    auto *smb = static_cast<WindowsSmbHandle *>(handle);
    return smb ? smb->file.size() : -1;
}

void WindowsSmbProvider::closeHandle(FileHandle *handle) {
    delete static_cast<WindowsSmbHandle *>(handle);
}

bool WindowsSmbProvider::setModifiedTime(const QString &path,
                                         const QDateTime &modified) {
    if (!modified.isValid() || !shareReady(path))
        return false;
    QFile file(uncFor(path));
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.setFileTime(modified, QFileDevice::FileModificationTime);
}

bool WindowsSmbProvider::remove(const QString &path) {
    if (!shareReady(path))
        return false;
    const QString unc = uncFor(path);
    return QFileInfo(unc).isDir() ? QDir().rmdir(unc) : QFile::remove(unc);
}

bool WindowsSmbProvider::mkdir(const QString &path) {
    return shareReady(path) && QDir().mkpath(uncFor(path));
}
