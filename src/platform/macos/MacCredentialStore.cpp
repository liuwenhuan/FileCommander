#include "CredentialStore.h"

#include <QProcess>

namespace {

const QString kService = QStringLiteral("com.aigutta.FileCommander");

PlatformResult invalidId() {
    return PlatformResult::failure(PlatformError::InvalidPath,
                                    QStringLiteral("Credential id is empty."));
}

struct SecurityResult {
    bool started = false;
    int exitCode = -1;
    QString standardOutput;
    QString standardError;
};

SecurityResult runSecurity(const QStringList &arguments, const QByteArray &standardInput = {}) {
    QProcess process;
    process.start(QStringLiteral("/usr/bin/security"), arguments);
    if (!process.waitForStarted())
        return {};
    process.write(standardInput);
    process.closeWriteChannel();
    if (!process.waitForFinished(-1))
        return {true, -1, {}, QStringLiteral("The security command did not finish.")};
    return {
        true,
        process.exitCode(),
        QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
        QString::fromUtf8(process.readAllStandardError()).trimmed(),
    };
}

PlatformResult securityFailure(const SecurityResult &result, const QString &action) {
    const QString detail = result.standardError.isEmpty()
                               ? QStringLiteral("exit code %1").arg(result.exitCode)
                               : result.standardError;
    return PlatformResult::failure(PlatformError::NativeFailure,
                                   QStringLiteral("%1: %2").arg(action, detail),
                                   result.exitCode);
}

} // namespace

PlatformResult CredentialStore::save(const QString &id, const QString &secret) {
    if (id.isEmpty())
        return invalidId();
    const SecurityResult result = runSecurity({
        QStringLiteral("add-generic-password"),
        QStringLiteral("-a"), id,
        QStringLiteral("-s"), kService,
        QStringLiteral("-U"),
        QStringLiteral("-w"),
    }, secret.toUtf8() + '\n');
    return result.started && result.exitCode == 0
               ? PlatformResult::success()
               : securityFailure(result, QStringLiteral("Keychain save"));
}

PlatformResult CredentialStore::load(const QString &id, QString *secret) {
    if (secret)
        secret->clear();
    if (id.isEmpty())
        return invalidId();
    const SecurityResult result = runSecurity({
        QStringLiteral("find-generic-password"),
        QStringLiteral("-a"), id,
        QStringLiteral("-s"), kService,
        QStringLiteral("-w"),
    });
    if (result.started && result.exitCode == 0) {
        if (secret)
            *secret = result.standardOutput;
        return PlatformResult::success();
    }
    if (result.standardError.contains(QStringLiteral("could not be found"),
                                      Qt::CaseInsensitive)) {
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("Credential was not found."),
                                       result.exitCode);
    }
    return securityFailure(result, QStringLiteral("Keychain lookup"));
}

PlatformResult CredentialStore::remove(const QString &id) {
    if (id.isEmpty())
        return invalidId();
    const SecurityResult result = runSecurity({
        QStringLiteral("delete-generic-password"),
        QStringLiteral("-a"), id,
        QStringLiteral("-s"), kService,
    });
    if (result.started && result.exitCode == 0)
        return PlatformResult::success();
    if (result.standardError.contains(QStringLiteral("could not be found"),
                                      Qt::CaseInsensitive)) {
        return PlatformResult::success();
    }
    return securityFailure(result, QStringLiteral("Keychain removal"));
}
