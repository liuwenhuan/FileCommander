#pragma once

#include <QString>

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

    static PlatformResult success() { return {}; }
    static PlatformResult failure(PlatformError code, QString message, qint64 nativeCode = 0) {
        return {false, code, std::move(message), nativeCode};
    }
};
