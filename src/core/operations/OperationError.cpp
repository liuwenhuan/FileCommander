#include "OperationError.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#endif

namespace {

OperationErrorCategory categoryForNativeCode(qint64 code)
{
#ifdef Q_OS_WIN
    switch (static_cast<DWORD>(code)) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return OperationErrorCategory::PermissionDenied;
    case ERROR_WRITE_PROTECT:
        return OperationErrorCategory::ReadOnlyFileSystem;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return OperationErrorCategory::DiskFull;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return OperationErrorCategory::FileInUse;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return OperationErrorCategory::PathNotFound;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return OperationErrorCategory::AlreadyExists;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        return OperationErrorCategory::InvalidName;
    case ERROR_FILENAME_EXCED_RANGE:
        return OperationErrorCategory::PathTooLong;
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETNAME_DELETED:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_SEM_TIMEOUT:
        return OperationErrorCategory::Network;
    case ERROR_CANCELLED:
    case ERROR_OPERATION_ABORTED:
        return OperationErrorCategory::Cancelled;
    default:
        return OperationErrorCategory::Io;
    }
#else
    switch (static_cast<int>(code)) {
    case EACCES:
    case EPERM:
        return OperationErrorCategory::PermissionDenied;
    case EROFS:
        return OperationErrorCategory::ReadOnlyFileSystem;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return OperationErrorCategory::DiskFull;
    case EBUSY:
#ifdef ETXTBSY
    case ETXTBSY:
#endif
        return OperationErrorCategory::FileInUse;
    case ENOENT:
    case ENODEV:
        return OperationErrorCategory::PathNotFound;
    case EEXIST:
        return OperationErrorCategory::AlreadyExists;
    case EINVAL:
        return OperationErrorCategory::InvalidName;
    case ENAMETOOLONG:
        return OperationErrorCategory::PathTooLong;
    case ENETDOWN:
    case ENETUNREACH:
    case ECONNRESET:
    case ETIMEDOUT:
        return OperationErrorCategory::Network;
    case ECANCELED:
    case EINTR:
        return OperationErrorCategory::Cancelled;
    default:
        return OperationErrorCategory::Io;
    }
#endif
}

} // namespace

QString operationErrorFallbackMessage(OperationErrorCategory category)
{
    const char *context = "OperationError";
    switch (category) {
    case OperationErrorCategory::PermissionDenied:
        return QCoreApplication::translate(context, "Permission denied.");
    case OperationErrorCategory::ReadOnlyFileSystem:
        return QCoreApplication::translate(context, "The destination is read-only.");
    case OperationErrorCategory::DiskFull:
        return QCoreApplication::translate(context, "There is not enough free space.");
    case OperationErrorCategory::FileInUse:
        return QCoreApplication::translate(context, "The file is in use by another process.");
    case OperationErrorCategory::PathNotFound:
        return QCoreApplication::translate(context, "The source or destination path was not found.");
    case OperationErrorCategory::AlreadyExists:
        return QCoreApplication::translate(context, "The destination already exists.");
    case OperationErrorCategory::InvalidName:
        return QCoreApplication::translate(context, "The file name is invalid.");
    case OperationErrorCategory::PathTooLong:
        return QCoreApplication::translate(context, "The file path is too long.");
    case OperationErrorCategory::Network:
        return QCoreApplication::translate(context, "A network error interrupted the operation.");
    case OperationErrorCategory::Cancelled:
        return QCoreApplication::translate(context, "The operation was cancelled.");
    case OperationErrorCategory::Io:
    default:
        return QCoreApplication::translate(context, "The file operation failed.");
    }
}

OperationError classifyNativeOperationError(OperationType operation,
                                            const QString &sourcePath,
                                            const QString &targetPath,
                                            qint64 nativeCode,
                                            bool localOperation,
                                            const QString &nativeMessage)
{
    OperationError error;
    error.operation = operation;
    error.category = categoryForNativeCode(nativeCode);
    error.sourcePath = sourcePath;
    error.targetPath = targetPath;
    error.nativeCode = nativeCode;
    error.remote = !localOperation;
    error.elevatable = localOperation
        && error.category == OperationErrorCategory::PermissionDenied;
    error.retryable = error.category != OperationErrorCategory::Cancelled;
    error.skippable = error.category != OperationErrorCategory::Cancelled;
    error.message = nativeMessage.trimmed();
    if (error.message.isEmpty())
        error.message = operationErrorFallbackMessage(error.category);
    return error;
}
