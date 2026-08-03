#include "WindowsSmbSession.h"

#include <QMutexLocker>

#include <Windows.h>
#include <Winnetwk.h>
#include <Lm.h>

WindowsSmbSession::~WindowsSmbSession() {
    disconnectOwned();
}

QString WindowsSmbSession::describe(unsigned long status) {
    wchar_t *buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t *>(&buffer), 0,
        nullptr);
    const QString text =
        size ? QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed()
             : QStringLiteral("Windows error %1").arg(status);
    if (buffer)
        LocalFree(buffer);
    return text;
}

WindowsSmbSession::Result WindowsSmbSession::classify(unsigned long status) {
    switch (status) {
    case NO_ERROR:
        return Result::Connected;

    // Windows already has a session to this target. Not an error: the existing
    // connection is usable, and tearing it down to re-make it with our own
    // credentials would disrupt whatever else is using it.
    //
    // ERROR_SESSION_CREDENTIAL_CONFLICT is the one that bites in practice --
    // "multiple connections to a server by the same user, using more than one
    // user name, are not allowed". Reporting it as an auth failure would prompt
    // for a password that Windows will refuse to apply anyway, so the honest
    // answer is that a session exists and we should use it.
    case ERROR_ALREADY_ASSIGNED:
    case ERROR_ALREADY_EXISTS:
    case ERROR_DEVICE_ALREADY_REMEMBERED:
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
        return Result::Connected;

    // The server rejected who we are (or that we are nobody). Answerable by
    // credentials, so the UI prompts rather than retrying the same dial.
    case ERROR_ACCESS_DENIED:
    case ERROR_INVALID_PASSWORD:
    case ERROR_LOGON_FAILURE:
    case ERROR_NO_SUCH_USER:
    case ERROR_ACCOUNT_DISABLED:
    case ERROR_ACCOUNT_EXPIRED:
    case ERROR_ACCOUNT_LOCKED_OUT:
    case ERROR_ACCOUNT_RESTRICTION:
    case ERROR_PASSWORD_EXPIRED:
    case ERROR_PASSWORD_MUST_CHANGE:
    case ERROR_INVALID_LOGON_HOURS:
    case ERROR_INVALID_WORKSTATION:
    case ERROR_LOGON_TYPE_NOT_GRANTED:
    case ERROR_NOT_AUTHENTICATED:
    case ERROR_TRUST_FAILURE:
    case ERROR_NO_LOGON_SERVERS:
    case ERROR_NO_TRUST_SAM_ACCOUNT:
    case ERROR_DOWNGRADE_DETECTED:
        return Result::AuthRequired;

    // Everything else -- unreachable host, bad name, no network, timeout, the
    // remote service being absent -- is a plain failure. Prompting for a
    // password would be a lie about what went wrong.
    default:
        return Result::Failed;
    }
}

QString WindowsSmbSession::existingSessionUser(const QString &host) {
    if (host.isEmpty())
        return {};
    // NetUseEnum reports this logon session's own connection table, which is
    // exactly the scope ERROR_SESSION_CREDENTIAL_CONFLICT is about: Windows
    // refuses a second session to one server for THIS user, so the conflicting
    // connection is necessarily one of these.
    const QString prefix = QStringLiteral("\\\\") + host + QLatin1Char('\\');
    USE_INFO_2 *buffer = nullptr;
    DWORD read = 0;
    DWORD total = 0;
    DWORD resume = 0;
    QString user;
    const NET_API_STATUS status =
        NetUseEnum(nullptr, 2, reinterpret_cast<LPBYTE *>(&buffer), MAX_PREFERRED_LENGTH,
                   &read, &total, &resume);
    if (status == NERR_Success || status == ERROR_MORE_DATA) {
        for (DWORD i = 0; i < read && user.isEmpty(); ++i) {
            if (!buffer[i].ui2_remote)
                continue;
            const QString remote = QString::fromWCharArray(buffer[i].ui2_remote);
            // Match the server, not one particular share: the conflicting
            // connection may be to any share on it (or to IPC$).
            if (!remote.startsWith(prefix, Qt::CaseInsensitive) &&
                remote.compare(QStringLiteral("\\\\") + host, Qt::CaseInsensitive) != 0)
                continue;
            if (!buffer[i].ui2_username)
                continue;
            const QString candidate = QString::fromWCharArray(buffer[i].ui2_username);
            if (!candidate.isEmpty())
                user = candidate; // empty means a guest/anonymous mount: say nothing
        }
    }
    if (buffer)
        NetApiBufferFree(buffer);
    return user;
}

bool WindowsSmbSession::lastConnectBorrowed() const {
    QMutexLocker lock(&m_mutex);
    return m_lastConnectBorrowed;
}

void WindowsSmbSession::setCredentials(const QString &user, const QString &password,
                                       bool anonymous) {
    QMutexLocker lock(&m_mutex);
    m_user = user;
    m_password = password;
    m_anonymous = anonymous;
}

WindowsSmbSession::Result WindowsSmbSession::connectTarget(const QString &uncTarget,
                                                           QString *error) {
    QMutexLocker lock(&m_mutex);
    if (m_ownedConnections.contains(uncTarget))
        return Result::Connected;
    if (m_borrowedConnections.contains(uncTarget)) {
        m_lastConnectBorrowed = true;
        return Result::Connected;
    }
    m_lastConnectBorrowed = false;

    NETRESOURCEW resource{};
    // IPC$ is a pipe share, not a disk. RESOURCETYPE_ANY covers both it and a
    // real disk share; declaring DISK for IPC$ makes Windows refuse the call
    // with ERROR_BAD_DEV_TYPE before it ever reaches the server.
    resource.dwType = RESOURCETYPE_ANY;
    resource.lpRemoteName =
        const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(uncTarget.utf16()));

    // Anonymous means "no credentials at all", which is not the same as an empty
    // user name: passing an empty string makes Windows fall back to the logged-in
    // user's token, so an anonymous browse would silently authenticate as the
    // current user and mask the very rejection the caller needs to see.
    const wchar_t *user = m_anonymous || m_user.isEmpty()
                              ? nullptr
                              : reinterpret_cast<const wchar_t *>(m_user.utf16());
    const wchar_t *password =
        m_anonymous ? L"" : reinterpret_cast<const wchar_t *>(m_password.utf16());

    const DWORD status = WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY);
    const Result result = classify(status);
    if (result == Result::Connected) {
        if (status == NO_ERROR) {
            m_ownedConnections.insert(uncTarget);
        } else {
            // Pre-existing; not ours to cancel, and it carries whatever identity
            // it was made under -- which the caller has to be able to surface.
            m_borrowedConnections.insert(uncTarget);
            m_lastConnectBorrowed = true;
        }
        return result;
    }
    if (error)
        *error = describe(status);
    return result;
}

WindowsSmbSession::Result WindowsSmbSession::connectToServer(const QString &host,
                                                             QString *error) {
    if (host.isEmpty()) {
        if (error)
            *error = QStringLiteral("No SMB server has been configured.");
        return Result::Failed;
    }
    return connectTarget(QStringLiteral("\\\\") + host + QStringLiteral("\\IPC$"), error);
}

WindowsSmbSession::Result WindowsSmbSession::ensureConnected(const QString &uncShare,
                                                             QString *error) {
    return connectTarget(uncShare, error);
}

bool WindowsSmbSession::holdsConnection() const {
    QMutexLocker lock(&m_mutex);
    return !m_ownedConnections.isEmpty() || !m_borrowedConnections.isEmpty();
}

void WindowsSmbSession::disconnectOwned() {
    QMutexLocker lock(&m_mutex);
    for (const QString &target : m_ownedConnections)
        WNetCancelConnection2W(reinterpret_cast<const wchar_t *>(target.utf16()), 0, FALSE);
    m_ownedConnections.clear();
    // Borrowed sessions are left alone on purpose -- see the header.
    m_borrowedConnections.clear();
    m_lastConnectBorrowed = false;
}
