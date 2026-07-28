#include "WindowsSmbSession.h"

#include <QMutexLocker>

#include <Windows.h>
#include <Winnetwk.h>

namespace {
QString winError(DWORD code) {
    wchar_t *buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    const QString text =
        size ? QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed()
             : QStringLiteral("Windows error %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return text;
}
}

WindowsSmbSession::~WindowsSmbSession() {
    disconnectOwned();
}

void WindowsSmbSession::setCredentials(const QString &user, const QString &password,
                                       bool anonymous) {
    QMutexLocker lock(&m_mutex);
    m_user = user;
    m_password = password;
    m_anonymous = anonymous;
}

bool WindowsSmbSession::ensureConnected(const QString &uncShare, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (m_ownedConnections.contains(uncShare))
        return true;

    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = const_cast<wchar_t *>(
        reinterpret_cast<const wchar_t *>(uncShare.utf16()));
    const wchar_t *user = m_anonymous || m_user.isEmpty()
                              ? nullptr
                              : reinterpret_cast<const wchar_t *>(m_user.utf16());
    const wchar_t *password =
        m_anonymous ? nullptr : reinterpret_cast<const wchar_t *>(m_password.utf16());
    const DWORD result =
        WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY);
    if (result == NO_ERROR) {
        m_ownedConnections.insert(uncShare);
        return true;
    }
    if (result == ERROR_ALREADY_ASSIGNED || result == ERROR_ALREADY_EXISTS)
        return true;
    if (error)
        *error = winError(result);
    return false;
}

void WindowsSmbSession::disconnectOwned() {
    QMutexLocker lock(&m_mutex);
    for (const QString &share : m_ownedConnections)
        WNetCancelConnection2W(reinterpret_cast<const wchar_t *>(share.utf16()), 0, FALSE);
    m_ownedConnections.clear();
}
