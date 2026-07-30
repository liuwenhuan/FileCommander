#include "WindowsSmbProvider.h"
#include "ProviderPath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

QFile::Permissions permissionsFor(const QFileInfo &info) {
    return info.permissions();
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
    m_lastConnectAuthFailed = false;
    return true;
}

void WindowsSmbProvider::disconnect() {
    m_session.disconnectOwned();
    m_host.clear();
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

bool WindowsSmbProvider::ensureShareFor(const QString &path, QString *error) const {
    bool valid = false;
    const QStringList parts = providerSegments(path, &valid);
    if (!valid || parts.isEmpty())
        return valid;
    const QString share = QStringLiteral("\\\\") + m_host + QLatin1Char('\\') + parts.first();
    return m_session.ensureConnected(share, error);
}

QVector<FileInfo> WindowsSmbProvider::list(const QString &path, bool showHidden) const {
    QVector<FileInfo> result;
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/")) {
        SHARE_INFO_1 *buffer = nullptr;
        DWORD read = 0;
        DWORD total = 0;
        const NET_API_STATUS status = NetShareEnum(
            reinterpret_cast<LPWSTR>(const_cast<ushort *>(m_host.utf16())), 1,
            reinterpret_cast<LPBYTE *>(&buffer), MAX_PREFERRED_LENGTH, &read, &total,
            nullptr);
        if (status == NERR_Success || status == ERROR_MORE_DATA) {
            for (DWORD i = 0; i < read; ++i) {
                if (buffer[i].shi1_type != STYPE_DISKTREE)
                    continue;
                const QString name = QString::fromWCharArray(buffer[i].shi1_netname);
                result.append(FileInfo::fromFields(
                    QStringLiteral("/") + name, name, 0, {}, true, {}));
            }
        }
        if (buffer)
            NetApiBufferFree(buffer);
        return result;
    }

    if (!ensureShareFor(clean))
        return result;
    QDir dir(uncFor(clean));
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden)
        filters |= QDir::Hidden;
    for (const QFileInfo &info : dir.entryInfoList(filters, QDir::DirsFirst | QDir::Name)) {
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
    if (cleanPath(path) == QStringLiteral("/"))
        return true;
    return ensureShareFor(path) && QFileInfo(uncFor(path)).isDir();
}

QString WindowsSmbProvider::cleanPath(const QString &path) const {
    return normalizeProviderPath(path);
}

QString WindowsSmbProvider::parentPath(const QString &path) const {
    return parentProviderPath(path);
}

bool WindowsSmbProvider::exists(const QString &path) const {
    if (cleanPath(path) == QStringLiteral("/"))
        return isConnected();
    return ensureShareFor(path) && QFileInfo::exists(uncFor(path));
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
    if (!ensureShareFor(srcPath) || QFileInfo::exists(uncFor(dstPath)))
        return QFileInfo::exists(uncFor(dstPath)) ? RenameResult::AlreadyExists
                                                  : RenameResult::Failed;
    return QDir().rename(uncFor(srcPath), uncFor(dstPath)) ? RenameResult::Ok
                                                           : RenameResult::Failed;
}

FileHandle *WindowsSmbProvider::openRead(const QString &path) {
    if (!ensureShareFor(path))
        return nullptr;
    auto *handle = new WindowsSmbHandle(uncFor(path));
    if (!handle->file.open(QIODevice::ReadOnly)) {
        delete handle;
        return nullptr;
    }
    return handle;
}

FileHandle *WindowsSmbProvider::openWrite(const QString &path, bool truncate) {
    if (!ensureShareFor(path))
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
    if (!modified.isValid() || !ensureShareFor(path))
        return false;
    QFile file(uncFor(path));
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.setFileTime(modified, QFileDevice::FileModificationTime);
}

bool WindowsSmbProvider::remove(const QString &path) {
    if (!ensureShareFor(path))
        return false;
    const QString unc = uncFor(path);
    return QFileInfo(unc).isDir() ? QDir().rmdir(unc) : QFile::remove(unc);
}

bool WindowsSmbProvider::mkdir(const QString &path) {
    return ensureShareFor(path) && QDir().mkpath(uncFor(path));
}
