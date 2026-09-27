#include "privilege/PrivilegedFileOperation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <cerrno>
#include <cstdio>

namespace {

QJsonObject response(bool ok, PrivilegeStatus status, qint64 nativeCode,
                     const QString &message = {}) {
    return {
        {QStringLiteral("ok"), ok},
        {QStringLiteral("status"), static_cast<int>(status)},
        {QStringLiteral("nativeCode"), static_cast<double>(nativeCode)},
        {QStringLiteral("message"), message},
    };
}

bool exists(const QString &path) {
    return QFileInfo(path).exists() || QFileInfo(path).isSymLink();
}

int runTool(const QString &program, const QStringList &arguments, QString *errorMessage) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Could not start %1.").arg(program);
        return ENOENT;
    }
    if (!process.waitForFinished(-1)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 did not finish.").arg(program);
        return EIO;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (errorMessage)
            *errorMessage = detail.isEmpty()
                                ? QStringLiteral("%1 failed with exit code %2.")
                                      .arg(program)
                                      .arg(process.exitCode())
                                : detail;
        return process.exitCode() == 0 ? EIO : process.exitCode();
    }
    return 0;
}

PrivilegeResult perform(const PrivilegedOperationRequest &request) {
    const PrivilegeResult validation = validatePrivilegedOperationRequest(request);
    if (validation.status != PrivilegeStatus::Succeeded)
        return validation;

    const bool needsSource = request.kind != PrivilegedOperationKind::Mkdir;
    const bool needsTarget = request.kind != PrivilegedOperationKind::DeletePermanent;
    if ((needsSource && !exists(request.sourcePath)) ||
        (needsTarget && request.kind != PrivilegedOperationKind::Mkdir &&
         !QFileInfo(request.targetPath).absoluteDir().exists())) {
        return {PrivilegeStatus::Failed, ENOENT,
                QStringLiteral("A source or destination parent does not exist.")};
    }

    if (request.kind == PrivilegedOperationKind::Mkdir) {
        QString error;
        const int code = runTool(QStringLiteral("/bin/mkdir"),
                                 {QStringLiteral("-p"), QStringLiteral("--"),
                                  request.targetPath},
                                 &error);
        return code == 0
                   ? PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}}
                   : PrivilegeResult{PrivilegeStatus::Failed, code, error};
    }

    if (request.kind != PrivilegedOperationKind::DeletePermanent &&
        exists(request.targetPath)) {
        if (!request.overwrite)
            return {PrivilegeStatus::Failed, EEXIST,
                    QStringLiteral("The target path already exists.")};
        QString error;
        const int code = runTool(QStringLiteral("/bin/rm"),
                                 {QStringLiteral("-rf"), QStringLiteral("--"),
                                  request.targetPath},
                                 &error);
        if (code != 0)
            return {PrivilegeStatus::Failed, code, error};
    }

    QString program;
    QStringList arguments;
    switch (request.kind) {
    case PrivilegedOperationKind::Copy:
        program = QStringLiteral("/bin/cp");
        arguments = QStringList{QStringLiteral("-a"), QStringLiteral("--"),
                                request.sourcePath, request.targetPath};
        break;
    case PrivilegedOperationKind::Move:
    case PrivilegedOperationKind::Rename:
        program = QStringLiteral("/bin/mv");
        arguments =
            QStringList{QStringLiteral("--"), request.sourcePath, request.targetPath};
        break;
    case PrivilegedOperationKind::DeletePermanent:
        program = QStringLiteral("/bin/rm");
        arguments =
            QStringList{QStringLiteral("-rf"), QStringLiteral("--"), request.sourcePath};
        break;
    case PrivilegedOperationKind::Symlink:
        program = QStringLiteral("/bin/ln");
        arguments = QStringList{QStringLiteral("-s"), QStringLiteral("--"),
                                request.sourcePath, request.targetPath};
        break;
    case PrivilegedOperationKind::Mkdir:
        break;
    }

    QString error;
    const int code = runTool(program, arguments, &error);
    return code == 0
               ? PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}}
               : PrivilegeResult{PrivilegeStatus::Failed, code, error};
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        const QByteArray output =
            QJsonDocument(response(false, PrivilegeStatus::InvalidRequest, EINVAL,
                                    QStringLiteral("The helper received an invalid argument count.")))
                .toJson(QJsonDocument::Compact);
        fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
        fputc('\n', stdout);
        return 2;
    }

    PrivilegedOperationRequest request;
    const PrivilegeResult decoded =
        decodePrivilegedRequest(QByteArray(argv[1]), &request);
    const PrivilegeResult result = decoded.status == PrivilegeStatus::Succeeded
                                       ? perform(request)
                                       : decoded;
    const QByteArray output =
        QJsonDocument(response(result.status == PrivilegeStatus::Succeeded,
                                result.status, result.nativeCode, result.message))
            .toJson(QJsonDocument::Compact);
    fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return result.status == PrivilegeStatus::Succeeded ? 0 : 1;
}
