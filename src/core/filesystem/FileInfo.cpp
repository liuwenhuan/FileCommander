#include "FileInfo.h"

#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>

#include <string>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace {

#ifdef Q_OS_WIN
QString extendedWindowsPath(const QString &path) {
    QString native = QDir::toNativeSeparators(QDir::cleanPath(path));
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

QString sidText(PSID sid) {
    if (!sid)
        return {};

    DWORD nameChars = 0;
    DWORD domainChars = 0;
    SID_NAME_USE use;
    LookupAccountSidW(nullptr, sid, nullptr, &nameChars, nullptr, &domainChars, &use);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && nameChars > 0) {
        std::wstring name(nameChars, L'\0');
        std::wstring domain(domainChars, L'\0');
        if (LookupAccountSidW(nullptr, sid, name.data(), &nameChars, domain.data(),
                              &domainChars, &use)) {
            const QString account = QString::fromWCharArray(name.c_str());
            const QString domainName = QString::fromWCharArray(domain.c_str());
            return domainName.isEmpty() ? account
                                        : domainName + QLatin1Char('\\') + account;
        }
    }

    LPWSTR sidString = nullptr;
    if (ConvertSidToStringSidW(sid, &sidString)) {
        const QString result = QString::fromWCharArray(sidString);
        LocalFree(sidString);
        return result;
    }
    return {};
}
#endif

} // namespace

FileInfo::FileInfo(const QString &path) {
    QFileInfo qfi(path);
    m_name = qfi.fileName();
    m_path = qfi.absoluteFilePath();
    m_isDir = qfi.isDir();
    m_isSymLink = qfi.isSymLink();
    // Directories are treated as having no extension: the whole name is the
    // base. For files, split off the trailing suffix so the Name and Ext
    // columns show complementary halves (e.g. "photo" + "jpg").
    m_suffix = m_isDir ? QString() : suffixForName(m_name);
    m_baseName = m_isDir ? m_name : baseNameForName(m_name);
    m_size = qfi.size();
    m_modified = qfi.lastModified();
    m_created = qfi.birthTime();
    if (!m_created.isValid())
        m_created = qfi.metadataChangeTime(); // birth time isn't always available
    m_permissions = qfi.permissions();
#ifdef Q_OS_WIN
    // QFileInfo::owner()/group() commonly return empty on Windows, and resolving
    // ACL names for every directory entry is expensive. Defer it until a caller
    // actually asks (normally the Properties dialog).
    m_ownerId = -1;
    m_groupId = -1;
    m_ownerGroupResolved = m_path.isEmpty();
#else
    // The local filesystem resolves both numeric ids and names.
    m_ownerId = static_cast<int>(qfi.ownerId());
    m_groupId = static_cast<int>(qfi.groupId());
    m_owner = qfi.owner();
    m_group = qfi.group();
#endif
    // m_mimeType is intentionally left empty here; see mimeType() below.
}

const QString &FileInfo::owner() const {
    ensureOwnerGroupResolved();
    return m_owner;
}

const QString &FileInfo::group() const {
    ensureOwnerGroupResolved();
    return m_group;
}

void FileInfo::ensureOwnerGroupResolved() const {
    if (m_ownerGroupResolved)
        return;
    m_ownerGroupResolved = true;

#ifdef Q_OS_WIN
    if (m_path.isEmpty())
        return;

    QString path = extendedWindowsPath(m_path);
    std::wstring wide = path.toStdWString();
    PSID ownerSid = nullptr;
    PSID groupSid = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status =
        GetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
                              &ownerSid, &groupSid, nullptr, nullptr, &descriptor);
    if (status == ERROR_SUCCESS) {
        m_owner = sidText(ownerSid);
        m_group = sidText(groupSid);
    }
    if (descriptor)
        LocalFree(descriptor);
#endif
}

QString FileInfo::baseNameForName(const QString &name) {
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? name.left(dot) : name;
}

QString FileInfo::suffixForName(const QString &name) {
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? name.mid(dot + 1) : QString();
}

const QString &FileInfo::mimeType() const {
    // Scanning a large directory constructs thousands of FileInfos; running
    // MIME detection on each one there dominated the load time even though
    // most listings never read it. Compute on demand instead, then cache.
    if (m_mimeType.isNull() && !m_isDir && !m_path.isEmpty()) {
        static QMimeDatabase db;
        m_mimeType = db.mimeTypeForFile(m_path).name();
    }
    return m_mimeType;
}

FileInfo FileInfo::fromFields(const QString &path, const QString &name, qint64 size,
                              const QDateTime &modified, bool isDir,
                              QFile::Permissions permissions, int ownerId, int groupId,
                              const QString &owner, const QString &group) {
    FileInfo info;
    info.m_name = name;
    info.m_path = path;
    info.m_isDir = isDir;
    info.m_isSymLink = false;
    // Match the local constructor's split rule: directories have no extension
    // (the whole name is the base); files show base + trailing suffix.
    if (isDir) {
        info.m_suffix = QString();
        info.m_baseName = name;
    } else {
        info.m_suffix = suffixForName(name);
        info.m_baseName = baseNameForName(name);
    }
    info.m_size = size;
    info.m_modified = modified;
    // m_created intentionally left invalid: SFTP exposes no creation time.
    info.m_permissions = permissions;
    info.m_ownerId = ownerId;
    info.m_groupId = groupId;
    info.m_owner = owner;
    info.m_group = group;
    info.m_ownerGroupResolved = true;
    return info;
}

FileInfo FileInfo::makeParentEntry(const QString &parentPath, bool localFilesystem) {
    FileInfo info;
    if (localFilesystem) {
        // The parent really is a directory on this machine: read it, exactly as
        // before -- date, permissions and owner of ".." all come out right.
        info = FileInfo(parentPath);
    } else {
        // A server's directory, or a directory inside an archive. Everything we
        // could say about it beyond "it is the parent, and it is a directory"
        // would be about a same-named local directory, so say nothing.
        info.m_path = parentPath;
        info.m_isDir = true;
        info.m_permissionsKnown = false;
    }
    info.m_name = QStringLiteral("..");
    info.m_baseName = QStringLiteral("..");
    info.m_suffix.clear(); // a directory has no extension
    info.m_isParentEntry = true;
    return info;
}

QString FileInfo::permissionsString() const {
    if (!m_permissionsKnown)
        return {};
    QString s;
    s += m_isDir ? QLatin1Char('d') : (m_isSymLink ? QLatin1Char('l') : QLatin1Char('-'));
    s += (m_permissions & QFile::ReadOwner) ? QLatin1Char('r') : QLatin1Char('-');
    s += (m_permissions & QFile::WriteOwner) ? QLatin1Char('w') : QLatin1Char('-');
    s += (m_permissions & QFile::ExeOwner) ? QLatin1Char('x') : QLatin1Char('-');
    s += (m_permissions & QFile::ReadGroup) ? QLatin1Char('r') : QLatin1Char('-');
    s += (m_permissions & QFile::WriteGroup) ? QLatin1Char('w') : QLatin1Char('-');
    s += (m_permissions & QFile::ExeGroup) ? QLatin1Char('x') : QLatin1Char('-');
    s += (m_permissions & QFile::ReadOther) ? QLatin1Char('r') : QLatin1Char('-');
    s += (m_permissions & QFile::WriteOther) ? QLatin1Char('w') : QLatin1Char('-');
    s += (m_permissions & QFile::ExeOther) ? QLatin1Char('x') : QLatin1Char('-');
    return s;
}
