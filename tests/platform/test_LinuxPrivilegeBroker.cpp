#include <gtest/gtest.h>

#include "privilege/PrivilegeBroker.h"

#ifndef Q_OS_WIN

#include <cerrno>

namespace {

PrivilegedOperationRequest copyRequest()
{
    return {
        1,
        PrivilegedOperationKind::Copy,
        QStringLiteral("/source.txt"),
        QStringLiteral("/target.txt"),
        false,
    };
}

class ResolverScope {
public:
    explicit ResolverScope(PrivilegeBroker::LinuxExecutableResolver resolver)
    {
        PrivilegeBroker::setLinuxExecutableResolverForTesting(std::move(resolver));
    }
    ~ResolverScope() { PrivilegeBroker::resetLinuxExecutableResolverForTesting(); }
};

class LauncherScope {
public:
    explicit LauncherScope(PrivilegeBroker::LinuxPrivilegeLauncher launcher)
    {
        PrivilegeBroker::setLinuxPrivilegeLauncherForTesting(std::move(launcher));
    }
    ~LauncherScope() { PrivilegeBroker::resetLinuxPrivilegeLauncherForTesting(); }
};

QString trustedPath(const QString &name)
{
    return QStringLiteral("/trusted/") + name;
}

} // namespace

TEST(LinuxPrivilegeBrokerTest, IsUnavailableWhenTrustedPkexecIsAbsent)
{
    ResolverScope resolver([](const QString &) { return QString(); });
    EXPECT_FALSE(PrivilegeBroker::isAvailable());
}

TEST(LinuxPrivilegeBrokerTest, LaunchesTrustedSystemCopyWithoutApplicationHelper)
{
    ResolverScope resolver([](const QString &name) { return trustedPath(name); });
    PrivilegeBroker::LinuxPrivilegeLaunchRequest launched;
    LauncherScope launcher([&](const PrivilegeBroker::LinuxPrivilegeLaunchRequest &request) {
        launched = request;
        return PrivilegeBroker::LinuxPrivilegeLaunchResult{true, 0, 0, true};
    });

    const PrivilegeResult result = PrivilegeBroker::execute(copyRequest());

    EXPECT_EQ(result.status, PrivilegeStatus::Succeeded);
    EXPECT_EQ(launched.program, trustedPath(QStringLiteral("pkexec")));
    EXPECT_EQ(launched.arguments,
              QStringList({trustedPath(QStringLiteral("cp")), QStringLiteral("-a"),
                           QStringLiteral("--"), QStringLiteral("/source.txt"),
                           QStringLiteral("/target.txt")}));
    EXPECT_FALSE(launched.arguments.contains(QStringLiteral("--privileged-helper")));
}

TEST(LinuxPrivilegeBrokerTest, MapsDismissedPkexecPromptToCancelledResult)
{
    ResolverScope resolver([](const QString &name) { return trustedPath(name); });
    LauncherScope launcher([](const PrivilegeBroker::LinuxPrivilegeLaunchRequest &) {
        return PrivilegeBroker::LinuxPrivilegeLaunchResult{true, 0, 126, true};
    });

    const PrivilegeResult result = PrivilegeBroker::execute(copyRequest());
    EXPECT_EQ(result.status, PrivilegeStatus::Cancelled);
    EXPECT_EQ(result.nativeCode, 126);
}

TEST(LinuxPrivilegeBrokerTest, MapsRejectedPkexecAuthorizationToDeniedResult)
{
    ResolverScope resolver([](const QString &name) { return trustedPath(name); });
    LauncherScope launcher([](const PrivilegeBroker::LinuxPrivilegeLaunchRequest &) {
        return PrivilegeBroker::LinuxPrivilegeLaunchResult{true, 0, 127, true};
    });

    const PrivilegeResult result = PrivilegeBroker::execute(copyRequest());
    EXPECT_EQ(result.status, PrivilegeStatus::Denied);
    EXPECT_EQ(result.nativeCode, 127);
}

#else

TEST(LinuxPrivilegeBrokerTest, CompilesOutsideLinux)
{
    GTEST_SKIP() << "Linux pkexec behavior is covered only on Linux.";
}

#endif
