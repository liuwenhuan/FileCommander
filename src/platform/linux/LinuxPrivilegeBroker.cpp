#include "privilege/PrivilegeBroker.h"

#include <QFileInfo>
#include <QProcess>

#include <cerrno>
#include <utility>

namespace {

constexpr int kPkexecDismissed = 126;
constexpr int kPkexecDenied = 127;

PrivilegeBroker::LinuxPrivilegeLauncher &testLauncher()
{
    static PrivilegeBroker::LinuxPrivilegeLauncher launcher;
    return launcher;
}

PrivilegeBroker::LinuxExecutableResolver &testResolver()
{
    static PrivilegeBroker::LinuxExecutableResolver resolver;
    return resolver;
}

bool isTrustedSystemExecutable(const QString &path)
{
    const QFileInfo info(path);
    const QFileDevice::Permissions writableByOthers =
        QFileDevice::WriteGroup | QFileDevice::WriteOther;
    return info.isFile() && info.isExecutable() && info.ownerId() == 0 &&
           !(info.permissions() & writableByOthers);
}

QString systemExecutable(const QString &name)
{
    if (testResolver())
        return testResolver()(name);

    const QStringList candidates = {
        QStringLiteral("/usr/bin/") + name,
        QStringLiteral("/bin/") + name,
        QStringLiteral("/usr/sbin/") + name,
        QStringLiteral("/sbin/") + name,
    };
    for (const QString &candidate : candidates) {
        if (isTrustedSystemExecutable(candidate))
            return candidate;
    }
    return {};
}

PrivilegeResult failed(qint64 nativeCode, const QString &message)
{
    return {PrivilegeStatus::Failed, nativeCode, message};
}

PrivilegeResult resultFromExitCode(qint64 exitCode, const QString &message)
{
    if (exitCode == kPkexecDismissed)
        return {PrivilegeStatus::Cancelled, exitCode, message};
    if (exitCode == kPkexecDenied)
        return {PrivilegeStatus::Denied, exitCode, message};
    return failed(exitCode, message);
}

PrivilegeBroker::LinuxPrivilegeLaunchResult launchWithQProcess(
    const PrivilegeBroker::LinuxPrivilegeLaunchRequest &request,
    const PrivilegeBroker::CancelCheck &cancelled)
{
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(request.program, request.arguments);
    if (!process.waitForStarted())
        return {false, static_cast<qint64>(process.error()), 0, false};

    while (!process.waitForFinished(100)) {
        if (cancelled && cancelled()) {
            process.terminate();
            if (!process.waitForFinished(2000)) {
                process.kill();
                process.waitForFinished();
            }
            return {true, 0, kPkexecDismissed, true};
        }
        if (process.error() != QProcess::Timedout)
            return {false, static_cast<qint64>(process.error()), 0, false};
    }
    return {true, 0, static_cast<qint64>(process.exitCode()),
            process.exitStatus() == QProcess::NormalExit};
}

PrivilegeBroker::LinuxPrivilegeLaunchResult launch(
    const PrivilegeBroker::LinuxPrivilegeLaunchRequest &request,
    const PrivilegeBroker::CancelCheck &cancelled)
{
    if (testLauncher())
        return testLauncher()(request);
    return launchWithQProcess(request, cancelled);
}

PrivilegeResult commandForRequest(const PrivilegedOperationRequest &request,
                                  QString *program, QStringList *arguments)
{
    QString tool;
    switch (request.kind) {
    case PrivilegedOperationKind::Copy:
        tool = QStringLiteral("cp");
        *arguments = QStringList{QStringLiteral("-a")};
        break;
    case PrivilegedOperationKind::Move:
    case PrivilegedOperationKind::Rename:
        tool = QStringLiteral("mv");
        *arguments = QStringList{QStringLiteral("-f")};
        break;
    case PrivilegedOperationKind::DeletePermanent:
        tool = QStringLiteral("rm");
        *arguments = QStringList{QStringLiteral("-rf")};
        break;
    case PrivilegedOperationKind::Mkdir:
        tool = QStringLiteral("mkdir");
        break;
    case PrivilegedOperationKind::Symlink:
        tool = QStringLiteral("ln");
        *arguments = QStringList{QStringLiteral("-s")};
        break;
    }

    *program = systemExecutable(tool);
    if (program->isEmpty())
        return failed(ENOENT, QStringLiteral("A required system file tool is unavailable."));

    arguments->append(QStringLiteral("--"));
    if (!request.sourcePath.isEmpty())
        arguments->append(request.sourcePath);
    if (!request.targetPath.isEmpty())
        arguments->append(request.targetPath);
    return {PrivilegeStatus::Succeeded, 0, {}};
}

} // namespace

namespace PrivilegeBroker {

bool isAvailable()
{
    return !systemExecutable(QStringLiteral("pkexec")).isEmpty();
}

PrivilegeResult execute(const PrivilegedOperationRequest &request, CancelCheck cancelled)
{
    const PrivilegeResult validation = validatePrivilegedOperationRequest(request);
    if (validation.status != PrivilegeStatus::Succeeded)
        return validation;
    if ((QFileInfo::exists(request.targetPath) || QFileInfo(request.targetPath).isSymLink()) &&
        request.kind != PrivilegedOperationKind::DeletePermanent) {
        return failed(request.overwrite ? ENOTSUP : EEXIST,
                      request.overwrite
                          ? QStringLiteral("Administrator overwrite requires an installed helper.")
                          : QStringLiteral("The target path already exists."));
    }

    const QString pkexec = systemExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty())
        return failed(ENOENT, QStringLiteral("A trusted system pkexec is not available."));

    QString tool;
    QStringList toolArguments;
    const PrivilegeResult command = commandForRequest(request, &tool, &toolArguments);
    if (command.status != PrivilegeStatus::Succeeded)
        return command;

    const LinuxPrivilegeLaunchRequest launchRequest{pkexec, QStringList{tool} + toolArguments};
    const LinuxPrivilegeLaunchResult launchResult = launch(launchRequest, cancelled);
    if (!launchResult.started)
        return failed(launchResult.processError, QStringLiteral("The administrator operation could not be started."));
    if (!launchResult.exitedNormally)
        return failed(launchResult.exitCode, QStringLiteral("The administrator operation did not exit normally."));
    if (launchResult.exitCode == 0)
        return {PrivilegeStatus::Succeeded, 0, {}};
    return resultFromExitCode(launchResult.exitCode,
                              QStringLiteral("The administrator operation did not complete."));
}

void setLinuxPrivilegeLauncherForTesting(LinuxPrivilegeLauncher launcher)
{
    testLauncher() = std::move(launcher);
}

void resetLinuxPrivilegeLauncherForTesting()
{
    testLauncher() = {};
}

void setLinuxExecutableResolverForTesting(LinuxExecutableResolver resolver)
{
    testResolver() = std::move(resolver);
}

void resetLinuxExecutableResolverForTesting()
{
    testResolver() = {};
}

} // namespace PrivilegeBroker
