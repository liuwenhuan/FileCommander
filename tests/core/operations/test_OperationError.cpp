#include <gtest/gtest.h>

#include "operations/OperationError.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#endif

namespace {

qint64 accessDeniedCode()
{
#ifdef Q_OS_WIN
    return ERROR_ACCESS_DENIED;
#else
    return EACCES;
#endif
}

qint64 diskFullCode()
{
#ifdef Q_OS_WIN
    return ERROR_DISK_FULL;
#else
    return ENOSPC;
#endif
}

} // namespace

TEST(OperationErrorTest, AccessDeniedIsElevatablePermissionError)
{
    const OperationError error = classifyNativeOperationError(
        OperationType::Copy, QStringLiteral("C:/source.txt"),
        QStringLiteral("C:/target.txt"), accessDeniedCode(), true);

    EXPECT_EQ(error.category, OperationErrorCategory::PermissionDenied);
    EXPECT_TRUE(error.elevatable);
    EXPECT_FALSE(error.remote);
    EXPECT_EQ(error.sourcePath, QStringLiteral("C:/source.txt"));
    EXPECT_EQ(error.targetPath, QStringLiteral("C:/target.txt"));
}

TEST(OperationErrorTest, DiskFullIsNotElevatable)
{
    const OperationError error = classifyNativeOperationError(
        OperationType::Copy, QStringLiteral("C:/source.txt"),
        QStringLiteral("C:/target.txt"), diskFullCode(), true);

    EXPECT_EQ(error.category, OperationErrorCategory::DiskFull);
    EXPECT_FALSE(error.elevatable);
}

TEST(OperationErrorTest, RemotePermissionErrorIsNotElevatable)
{
    const OperationError error = classifyNativeOperationError(
        OperationType::Copy, QStringLiteral("sftp://host/source.txt"),
        QStringLiteral("sftp://host/target.txt"), accessDeniedCode(), false);

    EXPECT_EQ(error.category, OperationErrorCategory::PermissionDenied);
    EXPECT_FALSE(error.elevatable);
    EXPECT_TRUE(error.remote);
}
