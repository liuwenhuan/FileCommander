#pragma once

#include <QString>
#include <QStringList>

enum class PlatformError {
    None,
    InvalidPath,
    NotFound,
    PermissionDenied,
    Busy,
    Unsupported,
    NativeFailure
};

struct PlatformResult {
    bool ok = true;
    PlatformError code = PlatformError::None;
    QString message;
    qint64 nativeCode = 0;
    // Opaque locations the trash backend can use to restore this operation.
    QStringList undoEntries;

    static PlatformResult success(QStringList undoEntries = {}) {
        PlatformResult result;
        result.undoEntries = std::move(undoEntries);
        return result;
    }
    static PlatformResult failure(PlatformError code, QString message, qint64 nativeCode = 0) {
        return {false, code, std::move(message), nativeCode};
    }
};
