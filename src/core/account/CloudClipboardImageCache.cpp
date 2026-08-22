#include "CloudClipboardImageCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QVector>

#include <algorithm>

#include "config/PrivatePath.h"
#include "config/Settings.h"

namespace {

constexpr char kDataSuffix[] = ".bin";
constexpr char kMetadataSuffix[] = ".json";
constexpr char kPartSuffix[] = ".part";

bool safeMimeType(const QString &mimeType) {
    if (mimeType.isEmpty() || mimeType.size() > 200)
        return false;
    for (const QChar character : mimeType) {
        if (character == QLatin1Char('\r') || character == QLatin1Char('\n') ||
            character.unicode() < 0x20 || character.unicode() > 0x7e)
            return false;
    }
    return true;
}

bool writeAtomically(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit() && PrivatePath::restrictFile(path);
}

bool isSafeRegularFile(const QString &path) {
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymbolicLink();
}

} // namespace

CloudClipboardImageCache::CloudClipboardImageCache(const QString &directory)
    : m_directory(directory) {}

bool CloudClipboardImageCache::isSafeItemId(const QString &itemId) {
    return !normalizedItemId(itemId).isEmpty();
}

QString CloudClipboardImageCache::normalizedItemId(const QString &itemId) {
    const QString hex = itemId.trimmed().toLower();
    if (hex.size() == 32) {
        for (const QChar c : hex)
            if (!c.isDigit() && (c < QLatin1Char('a') || c > QLatin1Char('f')))
                return {};
        return hex;
    }
    // Accept the earlier canonical UUID form too, so existing cache tests/data
    // remain readable while server-issued ids use compact 32-character hex.
    const QUuid uuid(itemId);
    if (uuid.isNull())
        return {};
    const QString normalized = uuid.toString(QUuid::WithoutBraces);
    return itemId.compare(normalized, Qt::CaseInsensitive) == 0 ? normalized : QString();
}

QString CloudClipboardImageCache::directory() const {
    if (!m_directory.isEmpty())
        return QDir::cleanPath(QFileInfo(m_directory).absoluteFilePath());
    const QString config = Settings::configDir();
    return config.isEmpty() ? QString()
                            : QDir(config).filePath(QStringLiteral("cloud-clipboard/images"));
}

QString CloudClipboardImageCache::dataPath(const QString &normalizedItemId) const {
    return QDir(directory()).filePath(normalizedItemId + QLatin1String(kDataSuffix));
}

QString CloudClipboardImageCache::metadataPath(const QString &normalizedItemId) const {
    return QDir(directory()).filePath(normalizedItemId + QLatin1String(kMetadataSuffix));
}

bool CloudClipboardImageCache::ensureDirectory() const {
    const QString path = directory();
    if (path.isEmpty() || !QDir().mkpath(path))
        return false;
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymbolicLink())
        return false;
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || QDir::cleanPath(canonical) != QDir::cleanPath(info.absoluteFilePath()))
        return false;
    return PrivatePath::restrictDirectory(canonical);
}

bool CloudClipboardImageCache::store(const QString &itemId, const QByteArray &original,
                                     const QString &mimeType, const QDateTime &requestedExpiry) {
    const QString id = normalizedItemId(itemId);
    if (id.isEmpty() || original.size() > maximumImageBytes || !safeMimeType(mimeType) ||
        !ensureDirectory()) {
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime latest = now.addDays(maximumAgeDays);
    QDateTime expiresAt = requestedExpiry.isValid() ? requestedExpiry.toUTC() : latest;
    if (expiresAt > latest)
        expiresAt = latest;
    if (expiresAt <= now)
        return false;

    const QString data = dataPath(id);
    const QString metadata = metadataPath(id);
    if ((QFileInfo(data).exists() && QFileInfo(data).isSymbolicLink()) ||
        (QFileInfo(metadata).exists() && QFileInfo(metadata).isSymbolicLink())) {
        return false;
    }

    const QByteArray hash = QCryptographicHash::hash(original, QCryptographicHash::Sha256);
    if (!writeAtomically(data, original))
        return false;

    QJsonObject object;
    object.insert(QStringLiteral("itemId"), id);
    object.insert(QStringLiteral("mimeType"), mimeType);
    object.insert(QStringLiteral("size"), static_cast<double>(original.size()));
    object.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.toHex()));
    object.insert(QStringLiteral("expiresAt"), expiresAt.toString(Qt::ISODate));
    if (!writeAtomically(metadata, QJsonDocument(object).toJson(QJsonDocument::Compact))) {
        QFile::remove(data);
        return false;
    }
    cleanup();
    return true;
}

bool CloudClipboardImageCache::readEntry(const QString &id, Entry *entry, bool verifyHash) const {
    if (id.isEmpty() || !ensureDirectory())
        return false;
    const QString data = dataPath(id);
    const QString metadata = metadataPath(id);
    if (!isSafeRegularFile(data) || !isSafeRegularFile(metadata))
        return false;

    QFile metaFile(metadata);
    if (!metaFile.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(metaFile.readAll()).object();
    const QString storedItemId = normalizedItemId(object.value(QStringLiteral("itemId")).toString());
    const QString mimeType = object.value(QStringLiteral("mimeType")).toString();
    const qint64 size = object.value(QStringLiteral("size")).toVariant().toLongLong();
    const QByteArray hash = object.value(QStringLiteral("sha256")).toString().toLatin1();
    const QDateTime expiresAt = QDateTime::fromString(object.value(QStringLiteral("expiresAt")).toString(),
                                                       Qt::ISODate).toUTC();
    const QFileInfo info(data);
    if (storedItemId != id || !safeMimeType(mimeType) || size < 0 || size > maximumImageBytes ||
        info.size() != size || hash.size() != 64 || QDateTime::currentDateTimeUtc() >= expiresAt) {
        return false;
    }
    const QByteArray expectedHash = QByteArray::fromHex(hash);
    if (expectedHash.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256))
        return false;
    if (verifyHash) {
        QFile image(data);
        if (!image.open(QIODevice::ReadOnly) ||
            QCryptographicHash::hash(image.readAll(), QCryptographicHash::Sha256) != expectedHash) {
            return false;
        }
    }
    if (entry) {
        entry->itemId = id;
        entry->filePath = data;
        entry->mimeType = mimeType;
        entry->size = size;
        entry->sha256 = expectedHash;
        entry->expiresAt = expiresAt;
    }
    return true;
}

bool CloudClipboardImageCache::lookup(const QString &itemId, Entry *entry) const {
    const QString id = normalizedItemId(itemId);
    return readEntry(id, entry, true);
}

std::unique_ptr<QFile> CloudClipboardImageCache::openRead(const QString &itemId, Entry *entry) const {
    Entry found;
    if (!lookup(itemId, &found))
        return {};
    auto file = std::make_unique<QFile>(found.filePath);
    if (!file->open(QIODevice::ReadOnly))
        return {};
    if (entry)
        *entry = found;
    return file;
}

void CloudClipboardImageCache::cleanup() const {
    if (!ensureDirectory())
        return;

    struct Candidate {
        QString itemId;
        QString data;
        QString metadata;
        qint64 size;
        QDateTime lastUsed;
    };
    QVector<Candidate> candidates;
    QDir dir(directory());
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    QSet<QString> metadataIds;
    qint64 total = 0;
    for (const QFileInfo &file : files) {
        if (file.isSymbolicLink())
            continue;
        const QString name = file.fileName();
        if (!name.endsWith(QLatin1String(kMetadataSuffix)))
            continue;
        const QString id = name.left(name.size() - int(sizeof(kMetadataSuffix) - 1));
        if (!isSafeItemId(id))
            continue;
        metadataIds.insert(id);
        Entry entry;
        if (!readEntry(id, &entry, false)) {
            QFile::remove(metadataPath(id));
            QFile::remove(dataPath(id));
            continue;
        }
        candidates.append({id, entry.filePath, metadataPath(id), entry.size, file.lastModified()});
        total += entry.size;
    }

    for (const QFileInfo &file : files) {
        if (file.isSymbolicLink())
            continue;
        const QString name = file.fileName();
        const bool data = name.endsWith(QLatin1String(kDataSuffix));
        const bool part = name.endsWith(QLatin1String(kPartSuffix));
        const QString suffix = data ? QLatin1String(kDataSuffix) : QLatin1String(kPartSuffix);
        const QString id = (data || part) ? name.left(name.size() - suffix.size()) : QString();
        if ((data && isSafeItemId(id) && !metadataIds.contains(id)) ||
            (part && file.lastModified().daysTo(QDateTime::currentDateTimeUtc()) >= maximumAgeDays)) {
            QFile::remove(file.filePath());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.lastUsed < b.lastUsed;
    });
    for (const Candidate &candidate : candidates) {
        if (total <= maximumCacheBytes)
            break;
        if (QFile::remove(candidate.data)) {
            QFile::remove(candidate.metadata);
            total -= candidate.size;
        }
    }
}
