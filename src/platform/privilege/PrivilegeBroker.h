#pragma once

#include "PrivilegedFileOperation.h"

#include <functional>
#include <QStringList>

namespace PrivilegeBroker {

using CancelCheck = std::function<bool()>;

bool isAvailable();
PrivilegeResult execute(const PrivilegedOperationRequest &request,
                        CancelCheck cancelled = {});

#ifdef Q_OS_WIN
using WindowsPrivilegeExecutor = std::function<PrivilegeResult(
    const PrivilegedOperationRequest &request, const CancelCheck &cancelled)>;

void setWindowsPrivilegeExecutorForTesting(WindowsPrivilegeExecutor executor);
void resetWindowsPrivilegeExecutorForTesting();
#endif

#ifndef Q_OS_WIN
struct LinuxPrivilegeLaunchRequest {
    QString program;
    QStringList arguments;
};

struct LinuxPrivilegeLaunchResult {
    bool started = false;
    qint64 processError = 0;
    qint64 exitCode = 0;
    bool exitedNormally = false;
};

using LinuxPrivilegeLauncher = std::function<LinuxPrivilegeLaunchResult(
    const LinuxPrivilegeLaunchRequest &request)>;
using LinuxExecutableResolver = std::function<QString(const QString &name)>;

void setLinuxPrivilegeLauncherForTesting(LinuxPrivilegeLauncher launcher);
void resetLinuxPrivilegeLauncherForTesting();
void setLinuxExecutableResolverForTesting(LinuxExecutableResolver resolver);
void resetLinuxExecutableResolverForTesting();
#endif

} // namespace PrivilegeBroker
