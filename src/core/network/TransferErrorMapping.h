#pragma once

#include <QString>

#include "FileProvider.h"

namespace TransferErrorMapping {

struct Result {
    FileHandle::StreamError error = FileHandle::StreamError::None;
    QString detail;
};

Result curlError(int curlCode, const QString &detail = {});
Result webDavHttpError(long httpCode);
Result ftpResponseError(long responseCode);
Result sftpError(unsigned long sftpStatus, int sessionCode, const QString &detail = {});

} // namespace TransferErrorMapping
