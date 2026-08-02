#include "PrivatePath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <windows.h>

#include <vector>
#endif

namespace {

#ifdef Q_OS_WIN
QString extendedWindowsPath(const QString &path) {
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

bool restrictWindowsPath(const QString &path, bool directory) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    DWORD tokenInfoSize = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenInfoSize);
    std::vector<BYTE> tokenInfo(tokenInfoSize);
    const bool hasTokenUser = tokenInfoSize > 0 &&
                              GetTokenInformation(token, TokenUser, tokenInfo.data(), tokenInfoSize,
                                                  &tokenInfoSize);
    CloseHandle(token);
    if (!hasTokenUser)
        return false;

    const PSID userSid = reinterpret_cast<TOKEN_USER *>(tokenInfo.data())->User.Sid;
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(systemBuffer);
    BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
    DWORD administratorsSize = sizeof(administratorsBuffer);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer, &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administratorsBuffer,
                            &administratorsSize)) {
        return false;
    }

    EXPLICIT_ACCESS_W access[3] = {};
    const DWORD inheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    const PSID principals[] = {userSid, systemBuffer, administratorsBuffer};
    for (int i = 0; i < 3; ++i) {
        access[i].grfAccessPermissions = GENERIC_ALL;
        access[i].grfAccessMode = SET_ACCESS;
        access[i].grfInheritance = inheritance;
        access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[i].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        access[i].Trustee.ptstrName = static_cast<LPWSTR>(principals[i]);
    }

    PACL dacl = nullptr;
    if (SetEntriesInAclW(3, access, nullptr, &dacl) != ERROR_SUCCESS)
        return false;

    std::wstring widePath = extendedWindowsPath(path).toStdWString();
    const DWORD status = SetNamedSecurityInfoW(widePath.data(), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION |
                                                   PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, dacl, nullptr);
    LocalFree(dacl);
    return status == ERROR_SUCCESS;
}
#endif

bool restrictPath(const QString &path, bool directory) {
#ifdef Q_OS_WIN
    return restrictWindowsPath(path, directory);
#else
    const QFileDevice::Permissions permissions =
        directory ? QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                  : QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    return QFile::setPermissions(path, permissions);
#endif
}

} // namespace

namespace PrivatePath {

bool restrictDirectory(const QString &path) {
    return restrictPath(path, true);
}

bool restrictFile(const QString &path) {
    return restrictPath(path, false);
}

} // namespace PrivatePath
