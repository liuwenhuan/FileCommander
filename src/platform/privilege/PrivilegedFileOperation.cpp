#include "privilege/PrivilegeBroker.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

const QSet<QString> kAllowedKeys = {
    QStringLiteral("version"),
    QStringLiteral("kind"),
    QStringLiteral("sourcePath"),
    QStringLiteral("targetPath"),
    QStringLiteral("overwrite"),
};

PrivilegeResult succeeded()
{
    return {PrivilegeStatus::Succeeded, 0, {}};
}

PrivilegeResult invalidRequest(const QString &message)
{
    return {PrivilegeStatus::InvalidRequest, 0, message};
}

QString kindName(PrivilegedOperationKind kind)
{
    switch (kind) {
    case PrivilegedOperationKind::Copy:
        return QStringLiteral("Copy");
    case PrivilegedOperationKind::Move:
        return QStringLiteral("Move");
    case PrivilegedOperationKind::DeletePermanent:
        return QStringLiteral("DeletePermanent");
    case PrivilegedOperationKind::Mkdir:
        return QStringLiteral("Mkdir");
    case PrivilegedOperationKind::Rename:
        return QStringLiteral("Rename");
    case PrivilegedOperationKind::Symlink:
        return QStringLiteral("Symlink");
    }
    return {};
}

bool parseKind(const QString &name, PrivilegedOperationKind *kind)
{
    const QList<PrivilegedOperationKind> kinds = {
        PrivilegedOperationKind::Copy,
        PrivilegedOperationKind::Move,
        PrivilegedOperationKind::DeletePermanent,
        PrivilegedOperationKind::Mkdir,
        PrivilegedOperationKind::Rename,
        PrivilegedOperationKind::Symlink,
    };
    for (const PrivilegedOperationKind candidate : kinds) {
        if (kindName(candidate) == name) {
            *kind = candidate;
            return true;
        }
    }
    return false;
}

bool isAbsoluteLocalPath(const QString &path)
{
    if (path.isEmpty() || path.contains(QChar::Null) || path.contains(QStringLiteral("://")) ||
        !QDir::isAbsolutePath(path)) {
        return false;
    }

    QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.startsWith(QStringLiteral("//")))
        return false;
#ifdef Q_OS_WIN
    if (normalized.size() >= 2 && normalized.at(1) == QLatin1Char(':')) {
        const QString root = normalized.left(2) + QLatin1Char('/');
        if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root).utf16())) ==
            DRIVE_REMOTE) {
            return false;
        }
    }
#endif
    return true;
}

PrivilegeResult validateRequest(const PrivilegedOperationRequest &request)
{
    if (request.version != 1 || kindName(request.kind).isEmpty())
        return invalidRequest(QStringLiteral("Unsupported privileged operation request."));

    const bool needsSource = request.kind != PrivilegedOperationKind::Mkdir;
    const bool needsTarget = request.kind != PrivilegedOperationKind::DeletePermanent;
    if (needsSource != !request.sourcePath.isEmpty() || needsTarget != !request.targetPath.isEmpty())
        return invalidRequest(QStringLiteral("The operation has invalid path fields."));
    if ((needsSource && !isAbsoluteLocalPath(request.sourcePath)) ||
        (needsTarget && !isAbsoluteLocalPath(request.targetPath)))
        return invalidRequest(QStringLiteral("Privileged operations require absolute local paths."));
    if ((request.kind == PrivilegedOperationKind::DeletePermanent ||
         request.kind == PrivilegedOperationKind::Mkdir) && request.overwrite)
        return invalidRequest(QStringLiteral("The operation does not support overwrite."));
    return succeeded();
}

bool isStrictBase64(const QByteArray &encoded)
{
    if (encoded.isEmpty() || encoded.size() % 4 != 0)
        return false;
    const int firstPadding = encoded.indexOf('=');
    const int padding = firstPadding < 0 ? 0 : encoded.size() - firstPadding;
    if (padding > 2)
        return false;
    for (int index = encoded.size() - padding; index < encoded.size(); ++index) {
        if (encoded.at(index) != '=')
            return false;
    }
    for (int index = 0; index < encoded.size() - padding; ++index) {
        const char character = encoded.at(index);
        const bool alphabet = (character >= 'A' && character <= 'Z') ||
                              (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9') ||
                              character == '+' || character == '/';
        if (!alphabet) {
            return false;
        }
    }
    return true;
}

} // namespace

PrivilegeResult validatePrivilegedOperationRequest(const PrivilegedOperationRequest &request)
{
    return validateRequest(request);
}

QByteArray encodePrivilegedRequest(const PrivilegedOperationRequest &request)
{
    if (validateRequest(request).status != PrivilegeStatus::Succeeded)
        return {};
    const QJsonObject object = {
        {QStringLiteral("version"), request.version},
        {QStringLiteral("kind"), kindName(request.kind)},
        {QStringLiteral("sourcePath"), request.sourcePath},
        {QStringLiteral("targetPath"), request.targetPath},
        {QStringLiteral("overwrite"), request.overwrite},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64();
}

PrivilegeResult decodePrivilegedRequest(const QByteArray &encoded,
                                        PrivilegedOperationRequest *request)
{
    if (!isStrictBase64(encoded))
        return invalidRequest(QStringLiteral("The request is not valid Base64."));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromBase64(encoded), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalidRequest(QStringLiteral("The request is not valid JSON."));

    const QJsonObject object = document.object();
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!kAllowedKeys.contains(iterator.key()))
            return invalidRequest(QStringLiteral("The request contains an unknown field."));
    }
    if (!object.contains(QStringLiteral("version")) || !object.value(QStringLiteral("version")).isDouble() ||
        object.value(QStringLiteral("version")).toInt() != 1 ||
        !object.contains(QStringLiteral("kind")) || !object.value(QStringLiteral("kind")).isString())
        return invalidRequest(QStringLiteral("The request has an unsupported version or operation."));
    if (object.contains(QStringLiteral("sourcePath")) && !object.value(QStringLiteral("sourcePath")).isString())
        return invalidRequest(QStringLiteral("The source path is not a string."));
    if (object.contains(QStringLiteral("targetPath")) && !object.value(QStringLiteral("targetPath")).isString())
        return invalidRequest(QStringLiteral("The target path is not a string."));
    if (object.contains(QStringLiteral("overwrite")) && !object.value(QStringLiteral("overwrite")).isBool())
        return invalidRequest(QStringLiteral("The overwrite flag is not a boolean."));

    PrivilegedOperationRequest decoded;
    decoded.version = object.value(QStringLiteral("version")).toInt();
    if (!parseKind(object.value(QStringLiteral("kind")).toString(), &decoded.kind))
        return invalidRequest(QStringLiteral("The request has an unsupported operation."));
    decoded.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    decoded.targetPath = object.value(QStringLiteral("targetPath")).toString();
    decoded.overwrite = object.value(QStringLiteral("overwrite")).toBool(false);

    const PrivilegeResult result = validateRequest(decoded);
    if (result.status == PrivilegeStatus::Succeeded && request)
        *request = decoded;
    return result;
}
