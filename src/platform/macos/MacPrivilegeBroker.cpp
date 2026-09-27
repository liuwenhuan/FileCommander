#include "privilege/PrivilegeBroker.h"

#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <cstdio>
#include <utility>

namespace {

PrivilegeBroker::LinuxPrivilegeLauncher &testLauncher() {
    static PrivilegeBroker::LinuxPrivilegeLauncher launcher;
    return launcher;
}

PrivilegeBroker::LinuxExecutableResolver &testResolver() {
    static PrivilegeBroker::LinuxExecutableResolver resolver;
    return resolver;
}

QString helperPath() {
    const QString overridePath =
        qEnvironmentVariable("FILECOMMANDER_PRIVILEGED_HELPER");
    if (!overridePath.isEmpty() && QFileInfo(overridePath).isExecutable())
        return overridePath;

    const QString helperName =
        QStringLiteral("com.aigutta.FileCommander.privileged-helper");
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(applicationDir).filePath(QStringLiteral("../Library/LaunchServices/") + helperName),
        QDir(applicationDir).filePath(helperName),
    };
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable())
            return info.absoluteFilePath();
    }
    return {};
}

PrivilegeResult failed(OSStatus status, const QString &message) {
    return {PrivilegeStatus::Failed, static_cast<qint64>(status), message};
}

PrivilegeResult authorizationFailure(OSStatus status) {
    if (status == errAuthorizationCanceled || status == errAuthorizationDenied)
        return {PrivilegeStatus::Denied, static_cast<qint64>(status),
                QStringLiteral("Administrator authorization was denied.")};
    return failed(status, QStringLiteral("macOS could not authorize the administrator operation."));
}

} // namespace

namespace PrivilegeBroker {

bool isAvailable() {
    return !helperPath().isEmpty();
}

PrivilegeResult execute(const PrivilegedOperationRequest &request, CancelCheck cancelled) {
    const PrivilegeResult validation = validatePrivilegedOperationRequest(request);
    if (validation.status != PrivilegeStatus::Succeeded)
        return validation;
    if (cancelled && cancelled())
        return {PrivilegeStatus::Cancelled, ECANCELED,
                QStringLiteral("The operation was cancelled.")};

    const QString helper = helperPath();
    if (helper.isEmpty())
        return {PrivilegeStatus::Failed, ENOENT,
                QStringLiteral("The FileCommander administrator helper is not installed.")};

    AuthorizationRef authorization = nullptr;
    OSStatus status = AuthorizationCreate(nullptr, nullptr, kAuthorizationFlagDefaults,
                                          &authorization);
    if (status != errAuthorizationSuccess)
        return authorizationFailure(status);

    const QByteArray helperBytes = QFile::encodeName(helper);
    AuthorizationItem right{
        kAuthorizationRightExecute,
        static_cast<UInt32>(helperBytes.size()),
        const_cast<char *>(helperBytes.constData()),
        0,
    };
    AuthorizationRights rights{1, &right};
    status = AuthorizationCopyRights(
        authorization, &rights, nullptr,
        kAuthorizationFlagInteractionAllowed | kAuthorizationFlagPreAuthorize |
            kAuthorizationFlagExtendRights,
        nullptr);
    if (status != errAuthorizationSuccess) {
        AuthorizationFree(authorization, kAuthorizationFlagDefaults);
        return authorizationFailure(status);
    }

    const QByteArray encoded = encodePrivilegedRequest(request);
    if (encoded.isEmpty()) {
        AuthorizationFree(authorization, kAuthorizationFlagDefaults);
        return {PrivilegeStatus::InvalidRequest, EINVAL,
                QStringLiteral("The administrator request could not be encoded.")};
    }

    char *arguments[] = {const_cast<char *>(encoded.constData()), nullptr};
    FILE *pipe = nullptr;
    status = AuthorizationExecuteWithPrivileges(
        authorization, helperBytes.constData(), kAuthorizationFlagDefaults, arguments, &pipe);
    if (status != errAuthorizationSuccess) {
        AuthorizationFree(authorization, kAuthorizationFlagDefaults);
        return authorizationFailure(status);
    }

    QByteArray output;
    if (pipe) {
        char buffer[4096];
        while (!feof(pipe)) {
            const size_t count = fread(buffer, 1, sizeof(buffer), pipe);
            if (count > 0)
                output.append(buffer, static_cast<int>(count));
            if (ferror(pipe))
                break;
        }
        fclose(pipe);
    }
    AuthorizationFree(authorization, kAuthorizationFlagDefaults);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failed(EIO, QStringLiteral("The administrator helper returned no valid result."));

    const QJsonObject object = document.object();
    const auto resultStatus =
        static_cast<PrivilegeStatus>(object.value(QStringLiteral("status")).toInt(
            static_cast<int>(PrivilegeStatus::Failed)));
    const qint64 nativeCode =
        static_cast<qint64>(object.value(QStringLiteral("nativeCode")).toDouble());
    const QString message = object.value(QStringLiteral("message")).toString();
    return {resultStatus, nativeCode, message};
}

void setLinuxPrivilegeLauncherForTesting(LinuxPrivilegeLauncher launcher) {
    testLauncher() = std::move(launcher);
}

void resetLinuxPrivilegeLauncherForTesting() {
    testLauncher() = {};
}

void setLinuxExecutableResolverForTesting(LinuxExecutableResolver resolver) {
    testResolver() = std::move(resolver);
}

void resetLinuxExecutableResolverForTesting() {
    testResolver() = {};
}

} // namespace PrivilegeBroker
