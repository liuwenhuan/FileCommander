#include "TransferErrorMapping.h"

#include <curl/curl.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

namespace TransferErrorMapping {
namespace {

QString fallbackDetail(const QString &detail, const QString &fallback) {
    return detail.isEmpty() ? fallback : detail;
}

bool isCurlConnectionError(CURLcode code) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return true;
    default:
        return false;
    }
}

bool isSftpConnectionError(int code) {
    switch (code) {
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_CHANNEL_CLOSED:
        return true;
    default:
        return false;
    }
}

} // namespace

Result curlError(int curlCode, const QString &detail) {
    const QString fallback = QStringLiteral("curl error %1").arg(curlCode);
    return {isCurlConnectionError(static_cast<CURLcode>(curlCode))
                ? FileHandle::StreamError::ConnectionLost
                : FileHandle::StreamError::Other,
            fallbackDetail(detail, fallback)};
}

Result webDavHttpError(long httpCode) {
    const QString detail = QStringLiteral("HTTP %1").arg(httpCode);
    switch (httpCode) {
    case 507:
        return {FileHandle::StreamError::NoSpace, detail};
    case 401:
    case 403:
    case 407:
        return {FileHandle::StreamError::PermissionDenied, detail};
    case 423:
        // The peer still holds the per-path upload lock (a concurrent or
        // just-cancelled transfer). Transient: the caller retries shortly.
        return {FileHandle::StreamError::Locked, detail};
    default:
        return {FileHandle::StreamError::Other, detail};
    }
}

Result ftpResponseError(long responseCode) {
    const QString detail = QStringLiteral("FTP %1").arg(responseCode);
    switch (responseCode) {
    case 452:
    case 552:
        return {FileHandle::StreamError::NoSpace, detail};
    case 530:
    case 532:
        return {FileHandle::StreamError::PermissionDenied, detail};
    default:
        return {FileHandle::StreamError::Other, detail};
    }
}

Result sftpError(unsigned long sftpStatus, int sessionCode, const QString &detail) {
    const QString fallback = sftpStatus != LIBSSH2_FX_OK
                                 ? QStringLiteral("SFTP status %1").arg(sftpStatus)
                                 : QStringLiteral("libssh2 error %1").arg(sessionCode);
    const QString message = fallbackDetail(detail, fallback);

    switch (sftpStatus) {
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
    case LIBSSH2_FX_QUOTA_EXCEEDED:
        return {FileHandle::StreamError::NoSpace, message};
    case LIBSSH2_FX_PERMISSION_DENIED:
    case LIBSSH2_FX_WRITE_PROTECT:
        return {FileHandle::StreamError::PermissionDenied, message};
    case LIBSSH2_FX_NO_CONNECTION:
    case LIBSSH2_FX_CONNECTION_LOST:
        return {FileHandle::StreamError::ConnectionLost, message};
    default:
        return {isSftpConnectionError(sessionCode) ? FileHandle::StreamError::ConnectionLost
                                                    : FileHandle::StreamError::Other,
                message};
    }
}

} // namespace TransferErrorMapping
