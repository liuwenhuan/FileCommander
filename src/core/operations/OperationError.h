#pragma once

#include <QString>

enum class OperationType {
    Copy,
    Move,
    Delete,
    Mkdir,
    Rename,
};

enum class OperationErrorCategory {
    PermissionDenied,
    ReadOnlyFileSystem,
    DiskFull,
    FileInUse,
    PathNotFound,
    AlreadyExists,
    InvalidName,
    PathTooLong,
    Network,
    Io,
    Cancelled,
};

struct OperationError {
    OperationType operation = OperationType::Copy;
    OperationErrorCategory category = OperationErrorCategory::Io;
    QString sourcePath;
    QString targetPath;
    QString message;
    qint64 nativeCode = 0;
    bool retryable = true;
    bool skippable = true;
    bool elevatable = false;
    bool remote = false;
};

OperationError classifyNativeOperationError(OperationType operation,
                                            const QString &sourcePath,
                                            const QString &targetPath,
                                            qint64 nativeCode,
                                            bool localOperation,
                                            const QString &nativeMessage = {});

QString operationErrorFallbackMessage(OperationErrorCategory category);
