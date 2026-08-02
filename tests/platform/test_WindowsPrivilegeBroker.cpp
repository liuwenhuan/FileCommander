#include <gtest/gtest.h>

#include "privilege/PrivilegeBroker.h"

#include <windows.h>

namespace {

PrivilegedOperationRequest copyRequest()
{
    return {
        1,
        PrivilegedOperationKind::Copy,
        QStringLiteral("C:/source.txt"),
        QStringLiteral("C:/target.txt"),
        false,
    };
}

class ExecutorScope {
public:
    explicit ExecutorScope(PrivilegeBroker::WindowsPrivilegeExecutor executor)
    {
        PrivilegeBroker::setWindowsPrivilegeExecutorForTesting(std::move(executor));
    }

    ~ExecutorScope()
    {
        PrivilegeBroker::resetWindowsPrivilegeExecutorForTesting();
    }
};

} // namespace

TEST(WindowsPrivilegeBrokerTest, UsesSystemFileOperationBackendWithoutLaunchingTheApplication)
{
    const PrivilegedOperationRequest expected = copyRequest();
    int calls = 0;
    ExecutorScope executor([&](const PrivilegedOperationRequest &request,
                               const PrivilegeBroker::CancelCheck &) {
        ++calls;
        EXPECT_EQ(request.kind, expected.kind);
        EXPECT_EQ(request.sourcePath, expected.sourcePath);
        EXPECT_EQ(request.targetPath, expected.targetPath);
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });

    const PrivilegeResult result = PrivilegeBroker::execute(expected);

    EXPECT_EQ(result.status, PrivilegeStatus::Succeeded);
    EXPECT_EQ(calls, 1);
}

TEST(WindowsPrivilegeBrokerTest, ForwardsCancellationToSystemFileOperationBackend)
{
    bool cancellationObserved = false;
    ExecutorScope executor([&](const PrivilegedOperationRequest &,
                               const PrivilegeBroker::CancelCheck &cancelled) {
        cancellationObserved = cancelled && cancelled();
        return PrivilegeResult{PrivilegeStatus::Cancelled, ERROR_CANCELLED, {}};
    });

    const PrivilegeResult result = PrivilegeBroker::execute(copyRequest(), [] { return true; });

    EXPECT_EQ(result.status, PrivilegeStatus::Cancelled);
    EXPECT_TRUE(cancellationObserved);
}

TEST(WindowsPrivilegeBrokerTest, RejectsRemoteRequestsBeforeCallingSystemBackend)
{
    int calls = 0;
    ExecutorScope executor([&](const PrivilegedOperationRequest &,
                               const PrivilegeBroker::CancelCheck &) {
        ++calls;
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    PrivilegedOperationRequest request = copyRequest();
    request.sourcePath = QStringLiteral("//server/share/source.txt");

    const PrivilegeResult result = PrivilegeBroker::execute(request);

    EXPECT_EQ(result.status, PrivilegeStatus::InvalidRequest);
    EXPECT_EQ(calls, 0);
}
