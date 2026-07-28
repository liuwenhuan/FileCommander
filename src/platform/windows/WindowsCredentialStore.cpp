#include "CredentialStore.h"

#include <Windows.h>
#include <WinCred.h>

namespace {
QString targetName(const QString &id) {
    return QStringLiteral("FileCommander/connection/") + id;
}

PlatformResult invalidId() {
    return PlatformResult::failure(PlatformError::InvalidPath,
                                   QStringLiteral("Credential id is empty."));
}

PlatformResult windowsFailure(DWORD code, const QString &action) {
    PlatformError kind = PlatformError::NativeFailure;
    if (code == ERROR_NOT_FOUND)
        kind = PlatformError::NotFound;
    else if (code == ERROR_ACCESS_DENIED)
        kind = PlatformError::PermissionDenied;
    return PlatformResult::failure(
        kind, QStringLiteral("%1 failed with Windows error %2.").arg(action).arg(code),
        static_cast<qint64>(code));
}
}

PlatformResult CredentialStore::save(const QString &id, const QString &secret) {
    if (id.isEmpty())
        return invalidId();
    const QByteArray bytes = secret.toUtf8();
    if (bytes.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        return PlatformResult::failure(PlatformError::Unsupported,
                                       QStringLiteral("Credential is too large."));
    const QString target = targetName(id);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName =
        const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(target.utf16()));
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char *>(bytes.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t *>(L"FileCommander");
    if (!CredWriteW(&credential, 0))
        return windowsFailure(GetLastError(), QStringLiteral("CredWrite"));
    return PlatformResult::success();
}

PlatformResult CredentialStore::load(const QString &id, QString *secret) {
    if (secret)
        secret->clear();
    if (id.isEmpty())
        return invalidId();
    const QString target = targetName(id);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<const wchar_t *>(target.utf16()),
                   CRED_TYPE_GENERIC, 0, &credential))
        return windowsFailure(GetLastError(), QStringLiteral("CredRead"));
    if (secret) {
        *secret = QString::fromUtf8(
            reinterpret_cast<const char *>(credential->CredentialBlob),
            static_cast<int>(credential->CredentialBlobSize));
    }
    CredFree(credential);
    return PlatformResult::success();
}

PlatformResult CredentialStore::remove(const QString &id) {
    if (id.isEmpty())
        return invalidId();
    const QString target = targetName(id);
    if (!CredDeleteW(reinterpret_cast<const wchar_t *>(target.utf16()),
                     CRED_TYPE_GENERIC, 0)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND)
            return windowsFailure(code, QStringLiteral("CredDelete"));
    }
    return PlatformResult::success();
}
