#include <gtest/gtest.h>

#include <curl/curl.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

#include "TransferErrorMapping.h"

namespace {

using StreamError = FileHandle::StreamError;

TEST(TransferErrorMappingTest, WebDavStatusErrorsAreClassified) {
    EXPECT_EQ(TransferErrorMapping::webDavHttpError(507).error, StreamError::NoSpace);
    EXPECT_EQ(TransferErrorMapping::webDavHttpError(401).error, StreamError::PermissionDenied);
    EXPECT_EQ(TransferErrorMapping::webDavHttpError(403).error, StreamError::PermissionDenied);
    EXPECT_EQ(TransferErrorMapping::webDavHttpError(407).error, StreamError::PermissionDenied);

    const auto rejected = TransferErrorMapping::webDavHttpError(503);
    EXPECT_EQ(rejected.error, StreamError::Other);
    EXPECT_EQ(rejected.detail, QStringLiteral("HTTP 503"));
}

TEST(TransferErrorMappingTest, FtpStatusErrorsAreClassified) {
    EXPECT_EQ(TransferErrorMapping::ftpResponseError(452).error, StreamError::NoSpace);
    EXPECT_EQ(TransferErrorMapping::ftpResponseError(552).error, StreamError::NoSpace);
    EXPECT_EQ(TransferErrorMapping::ftpResponseError(530).error, StreamError::PermissionDenied);
    EXPECT_EQ(TransferErrorMapping::ftpResponseError(532).error, StreamError::PermissionDenied);

    const auto rejected = TransferErrorMapping::ftpResponseError(550);
    EXPECT_EQ(rejected.error, StreamError::Other);
    EXPECT_EQ(rejected.detail, QStringLiteral("FTP 550"));
}

TEST(TransferErrorMappingTest, CurlTransportErrorsAreClassified) {
    EXPECT_EQ(TransferErrorMapping::curlError(CURLE_COULDNT_RESOLVE_HOST).error,
              StreamError::ConnectionLost);
    EXPECT_EQ(TransferErrorMapping::curlError(CURLE_COULDNT_CONNECT).error,
              StreamError::ConnectionLost);
    EXPECT_EQ(TransferErrorMapping::curlError(CURLE_OPERATION_TIMEDOUT).error,
              StreamError::ConnectionLost);
    EXPECT_EQ(TransferErrorMapping::curlError(CURLE_RECV_ERROR).error,
              StreamError::ConnectionLost);

    const auto certificate = TransferErrorMapping::curlError(CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(certificate.error, StreamError::Other);
    EXPECT_EQ(certificate.detail,
              QStringLiteral("curl error %1").arg(static_cast<int>(CURLE_PEER_FAILED_VERIFICATION)));
}

TEST(TransferErrorMappingTest, SftpErrorsAreClassified) {
    EXPECT_EQ(TransferErrorMapping::sftpError(LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM, 0).error,
              StreamError::NoSpace);
    EXPECT_EQ(TransferErrorMapping::sftpError(LIBSSH2_FX_QUOTA_EXCEEDED, 0).error,
              StreamError::NoSpace);
    EXPECT_EQ(TransferErrorMapping::sftpError(LIBSSH2_FX_PERMISSION_DENIED, 0).error,
              StreamError::PermissionDenied);
    EXPECT_EQ(TransferErrorMapping::sftpError(LIBSSH2_FX_WRITE_PROTECT, 0).error,
              StreamError::PermissionDenied);
    EXPECT_EQ(TransferErrorMapping::sftpError(LIBSSH2_FX_CONNECTION_LOST, 0).error,
              StreamError::ConnectionLost);
    EXPECT_EQ(TransferErrorMapping::sftpError(LIBSSH2_FX_OK, LIBSSH2_ERROR_SOCKET_TIMEOUT).error,
              StreamError::ConnectionLost);

    const auto failed = TransferErrorMapping::sftpError(LIBSSH2_FX_FAILURE, 0);
    EXPECT_EQ(failed.error, StreamError::Other);
    EXPECT_EQ(failed.detail, QStringLiteral("SFTP status %1").arg(LIBSSH2_FX_FAILURE));
}

} // namespace
