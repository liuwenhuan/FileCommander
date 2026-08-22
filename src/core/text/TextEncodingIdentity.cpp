#include "TextEncodingIdentity.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "filesystem/FileProvider.h"
#include "filesystem/ProviderPath.h"

namespace {
QString encodeFields(const QString &kind, const QStringList &fields) {
    QByteArray serialized;
    QDataStream stream(&serialized, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << kind << fields;
    return QString::fromLatin1(
        serialized.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
} // namespace

namespace fc::TextEncodingIdentity {

QString localPath(const QString &path) {
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    QString normalized = info.canonicalFilePath();
    if (normalized.isEmpty())
        normalized = info.absoluteFilePath();
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
    return normalized.isEmpty() ? QString() : encodeFields(QStringLiteral("local-v1"), {normalized});
}

QString remotePath(const RemoteLocation &location, const QString &providerPath) {
    if (!location.isValid() || providerPath.isEmpty())
        return {};
    return encodeFields(QStringLiteral("remote-v1"),
                        {location.scheme.toLower(), location.host.toLower(),
                         QString::number(location.port), location.user,
                         ProviderPath::normalizeRooted(providerPath)});
}

QString archiveEntry(const QString &containerIdentity, const QString &entryPath) {
    if (containerIdentity.isEmpty() || entryPath.isEmpty())
        return {};
    return encodeFields(QStringLiteral("archive-entry-v1"),
                        {containerIdentity, ProviderPath::normalizeRooted(entryPath)});
}

} // namespace fc::TextEncodingIdentity
