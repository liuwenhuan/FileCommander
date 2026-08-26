#include "ClipboardHistoryStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPixmap>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTextDocument>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <filesystem>

#include "config/PrivatePath.h"
#include "config/Settings.h"

namespace {

constexpr int kManifestVersion = 1;

QString normalizedId(const QString &id) {
    const QUuid uuid(id);
    if (uuid.isNull())
        return {};
    const QString normalized = uuid.toString(QUuid::WithoutBraces);
    return id.compare(normalized, Qt::CaseInsensitive) == 0 ? normalized : QString();
}

QByteArray sha256(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool isPrivateOrFileMime(const QMimeData *mime) {
    if (!mime || mime->hasUrls())
        return true;
    for (const QString &format : mime->formats()) {
        const QString lower = format.toLower();
        if (lower == QLatin1String("text/uri-list") || lower.contains(QLatin1String("file")) ||
            lower.contains(QLatin1String("password")) || lower.contains(QLatin1String("secret")) ||
            lower.contains(QLatin1String("private")) || lower.contains(QLatin1String("gnome-copied")) ||
            lower.contains(QLatin1String("kde-cutselection"))) {
            return true;
        }
    }
    return false;
}

bool hasPlainTextFormat(const QMimeData *mime) {
    for (const QString &format : mime->formats()) {
        if (format.compare(QLatin1String("text/plain"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString capturedText(const QMimeData *mime) {
    if (hasPlainTextFormat(mime)) {
        const QString plain = mime->text();
        if (!plain.isEmpty() && plain.toUtf8().size() <= ClipboardHistoryStore::maximumTextBytes)
            return plain;
    }
    if (mime->hasHtml()) {
        QTextDocument document;
        document.setHtml(mime->html());
        return document.toPlainText();
    }
    return mime->text();
}

bool acceptableImageSize(const QSize &size) {
    return size.isValid() && size.width() > 0 && size.height() > 0 &&
           qint64(size.width()) * size.height() <= ClipboardHistoryStore::maximumPixels;
}

QImage imageFromMimeData(const QMimeData *mime) {
    if (!mime)
        return {};

    const QVariant imageData = mime->imageData();
    if (imageData.canConvert<QImage>()) {
        const QImage image = qvariant_cast<QImage>(imageData);
        if (!image.isNull())
            return image;
    }
    if (imageData.canConvert<QPixmap>()) {
        const QPixmap pixmap = qvariant_cast<QPixmap>(imageData);
        if (!pixmap.isNull())
            return pixmap.toImage();
    }

    for (const QString &format : mime->formats()) {
        if (!format.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
            continue;
        const QByteArray bytes = mime->data(format);
        if (bytes.isEmpty() || bytes.size() > ClipboardHistoryStore::maximumImageBytes)
            continue;
        const QImage image = QImage::fromData(bytes);
        if (!image.isNull())
            return image;
    }

    const QList<QUrl> urls = mime->urls();
    if (urls.size() != 1 || !urls.first().isLocalFile())
        return {};
    const QString path = urls.first().toLocalFile();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymbolicLink() || info.size() <= 0 ||
        info.size() > ClipboardHistoryStore::maximumImageBytes) {
        return {};
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize declaredSize = reader.size();
    if (declaredSize.isValid() && !acceptableImageSize(declaredSize))
        return {};
    return reader.read();
}

bool sameHistoryIdentity(const ClipboardHistoryRecord &left, const ClipboardHistoryRecord &right) {
    return left.sha256 == right.sha256 && left.kind == right.kind && left.origin == right.origin &&
           (left.origin != ClipboardRecordOrigin::Incoming ||
            left.sourceDeviceId == right.sourceDeviceId);
}

QByteArray encodePng(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return bytes;
}

bool writeAtomically(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return false;
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
#endif
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit() && PrivatePath::restrictFile(path);
}

bool safeRegularFile(const QString &path) {
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymbolicLink();
}

bool isLegacyCacheItemId(const QString &id) {
    const QString hex = id.toLower();
    if (hex.size() == 32) {
        for (const QChar character : hex)
            if (!character.isDigit() && (character < QLatin1Char('a') || character > QLatin1Char('f')))
                return false;
        return true;
    }
    const QUuid uuid(id);
    return !uuid.isNull() && id.compare(uuid.toString(QUuid::WithoutBraces), Qt::CaseInsensitive) == 0;
}

bool isLegacyCacheFileName(const QString &name) {
    static const QStringList suffixes = {QStringLiteral(".bin"), QStringLiteral(".json"),
                                         QStringLiteral(".part")};
    for (const QString &suffix : suffixes) {
        if (name.endsWith(suffix) && isLegacyCacheItemId(name.left(name.size() - suffix.size())))
            return true;
    }
    return false;
}

bool removePathEntry(const QString &path) {
    if (QFile::remove(path))
        return true;
    std::error_code error;
    const std::filesystem::path nativePath(path.toStdWString());
    if (std::filesystem::remove(nativePath, error))
        return true;
    error.clear();
    const bool exists = std::filesystem::exists(nativePath, error);
    return !exists && !error;
}

// The old cache was a flat directory. Move a normal root aside first so a link
// swapped in at its original path cannot redirect a later child unlink. Then
// only inspect direct entries: symlinks/reparse points are unlinked, recognized
// cache files are unlinked, and unexpected entries are left untouched.
bool removeLegacyCacheDirectory(const QString &path) {
    const QFileInfo root(path);
    if (!root.exists() && !root.isSymbolicLink())
        return true;
    if (root.isSymbolicLink())
        return removePathEntry(path);
    if (!root.isDir())
        return false;

    const QString staged = QFileInfo(path).dir().filePath(
        QStringLiteral(".images-retiring-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().rename(path, staged))
        return false;

    const QFileInfo stagedRoot(staged);
    if (stagedRoot.isSymbolicLink())
        return removePathEntry(staged);
    if (!stagedRoot.isDir()) {
        QDir().rename(staged, path);
        return false;
    }

    QDir directory(staged);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymbolicLink()) {
            removePathEntry(entry.filePath());
        } else if (entry.isFile() && isLegacyCacheFileName(entry.fileName())) {
            QFile::remove(entry.filePath());
        }
    }
    if (removePathEntry(staged))
        return true;
    QDir().rename(staged, path);
    return false;
}

} // namespace

ClipboardHistoryStore::ClipboardHistoryStore(const QString &configDirectory)
    : m_configDirectory(configDirectory.isEmpty() ? Settings::configDir() : configDirectory) {
    load();
}

bool ClipboardHistoryStore::captureFromMimeData(const QMimeData *mime, ClipboardCapture *capture) {
    const QImage image = imageFromMimeData(mime);
    if (!image.isNull()) {
        const QByteArray encoded = encodePng(image);
        if (!acceptableImageSize(image.size()) || encoded.isEmpty() ||
            encoded.size() > maximumImageBytes) {
            return false;
        }
        if (capture) {
            capture->kind = ClipboardRecordKind::Image;
            capture->text.clear();
            capture->image = image;
            capture->mime = QStringLiteral("image/png");
        }
        return true;
    }

    if (isPrivateOrFileMime(mime))
        return false;
    if (!mime->hasText() && !mime->hasHtml())
        return false;
    const QString text = capturedText(mime);
    if (text.isEmpty() || text.toUtf8().size() > maximumTextBytes)
        return false;
    if (capture) {
        capture->kind = ClipboardRecordKind::Text;
        capture->text = text;
        capture->image = QImage();
        capture->mime = QStringLiteral("text/plain");
    }
    return true;
}

QString ClipboardHistoryStore::directory() const {
    return m_configDirectory.isEmpty() ? QString()
                                       : QDir(m_configDirectory).filePath(
                                             QStringLiteral("cloud-clipboard/history"));
}

QString ClipboardHistoryStore::imagesDirectory() const {
    return QDir(directory()).filePath(QStringLiteral("images"));
}

QString ClipboardHistoryStore::legacyImagesDirectory() const {
    return QDir(m_configDirectory).filePath(QStringLiteral("cloud-clipboard/images"));
}

QString ClipboardHistoryStore::manifestPath() const {
    return QDir(directory()).filePath(QStringLiteral("manifest.json"));
}

QString ClipboardHistoryStore::imagePathFor(const QString &id) const {
    const QString normalized = normalizedId(id);
    return normalized.isEmpty() ? QString()
                                : QDir(imagesDirectory()).filePath(normalized + QStringLiteral(".bin"));
}

bool ClipboardHistoryStore::ensureDirectories() const {
    const QString root = directory();
    const QString images = imagesDirectory();
    if (root.isEmpty() || !QDir().mkpath(images))
        return false;

    const QFileInfo rootInfo(root);
    const QFileInfo imagesInfo(images);
    if (!rootInfo.isDir() || rootInfo.isSymbolicLink() || !imagesInfo.isDir() ||
        imagesInfo.isSymbolicLink()) {
        return false;
    }
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString canonicalImages = imagesInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalImages.isEmpty() ||
        QDir::cleanPath(canonicalRoot) != QDir::cleanPath(rootInfo.absoluteFilePath()) ||
        QDir::cleanPath(canonicalImages) != QDir::cleanPath(imagesInfo.absoluteFilePath())) {
        return false;
    }
    return PrivatePath::restrictDirectory(canonicalRoot) &&
           PrivatePath::restrictDirectory(canonicalImages);
}

void ClipboardHistoryStore::removeLegacyImagesDirectory() const {
    removeLegacyCacheDirectory(legacyImagesDirectory());
}

bool ClipboardHistoryStore::saveManifest(const QVector<ClipboardHistoryRecord> &records) const {
    QJsonArray entries;
    for (const ClipboardHistoryRecord &record : records) {
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), record.id);
        entry.insert(QStringLiteral("origin"), record.origin == ClipboardRecordOrigin::Incoming
                                                   ? QStringLiteral("incoming")
                                                   : QStringLiteral("local"));
        entry.insert(QStringLiteral("kind"), record.kind == ClipboardRecordKind::Image
                                                 ? QStringLiteral("image")
                                                 : QStringLiteral("text"));
        entry.insert(QStringLiteral("text"), record.text);
        entry.insert(QStringLiteral("mime"), record.mime);
        entry.insert(QStringLiteral("sha256"), record.sha256);
        entry.insert(QStringLiteral("sourceDeviceId"), record.sourceDeviceId);
        entry.insert(QStringLiteral("sourceDeviceName"), record.sourceDeviceName);
        entry.insert(QStringLiteral("size"), static_cast<double>(record.size));
        entry.insert(QStringLiteral("width"), record.width);
        entry.insert(QStringLiteral("height"), record.height);
        entry.insert(QStringLiteral("created"), record.created.toUTC().toString(Qt::ISODateWithMs));
        entries.append(entry);
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), kManifestVersion);
    manifest.insert(QStringLiteral("records"), entries);
    return writeAtomically(manifestPath(), QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

void ClipboardHistoryStore::cleanupOrphanImages() const {
    QSet<QString> retained;
    for (const ClipboardHistoryRecord &record : m_records) {
        if (record.kind == ClipboardRecordKind::Image)
            retained.insert(QFileInfo(record.imagePath).fileName());
    }

    QDir images(imagesDirectory());
    const QFileInfoList files = images.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &file : files) {
        if (!file.isSymbolicLink() && !retained.contains(file.fileName()))
            QFile::remove(file.filePath());
    }
}

bool ClipboardHistoryStore::load() {
    m_records.clear();
    m_loaded = false;
    if (!ensureDirectories())
        return false;

    const QString path = manifestPath();
    QFile manifest(path);
    if (!manifest.exists()) {
        if (!saveManifest({}))
            return false;
        m_loaded = true;
        cleanupOrphanImages();
        removeLegacyImagesDirectory();
        return true;
    }
    if (!safeRegularFile(path) || !manifest.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll(), &error);
    manifest.close();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("version")).toInt() != kManifestVersion ||
        !document.object().value(QStringLiteral("records")).isArray()) {
        const QString quarantined = path + QStringLiteral(".corrupt");
        QFile::remove(quarantined);
        if (!QFile::rename(path, quarantined) || !PrivatePath::restrictFile(quarantined) ||
            !saveManifest({})) {
            return false;
        }
        m_loaded = true;
        cleanupOrphanImages();
        removeLegacyImagesDirectory();
        return true;
    }

    QSet<QString> ids;
    const QJsonArray entries = document.object().value(QStringLiteral("records")).toArray();
    for (const QJsonValue &value : entries) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        ClipboardHistoryRecord record;
        record.id = normalizedId(entry.value(QStringLiteral("id")).toString());
        record.origin = entry.value(QStringLiteral("origin")).toString() == QLatin1String("incoming")
                            ? ClipboardRecordOrigin::Incoming
                            : ClipboardRecordOrigin::Local;
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        record.kind = kind == QLatin1String("image") ? ClipboardRecordKind::Image
                                                       : ClipboardRecordKind::Text;
        record.text = entry.value(QStringLiteral("text")).toString();
        record.mime = entry.value(QStringLiteral("mime")).toString();
        record.sha256 = entry.value(QStringLiteral("sha256")).toString().toLower();
        record.sourceDeviceId = entry.value(QStringLiteral("sourceDeviceId")).toString();
        record.sourceDeviceName = entry.value(QStringLiteral("sourceDeviceName")).toString();
        record.size = entry.value(QStringLiteral("size")).toVariant().toLongLong();
        record.width = entry.value(QStringLiteral("width")).toInt();
        record.height = entry.value(QStringLiteral("height")).toInt();
        const QString createdText = entry.value(QStringLiteral("created")).toString();
        record.created = QDateTime::fromString(createdText, Qt::ISODateWithMs);
        // Version 1 manifests written before millisecond precision used ISODate.
        // Keep them readable while all new manifests use ISODateWithMs.
        if (!record.created.isValid())
            record.created = QDateTime::fromString(createdText, Qt::ISODate);
        record.created = record.created.toUTC();
        if (record.id.isEmpty() || ids.contains(record.id) || !record.created.isValid() ||
            record.sha256.size() != 64) {
            continue;
        }

        bool valid = false;
        if (record.kind == ClipboardRecordKind::Text) {
            const QByteArray bytes = record.text.toUtf8();
            valid = record.mime == QLatin1String("text/plain") && !record.text.isEmpty() &&
                    bytes.size() <= maximumTextBytes && record.size == bytes.size() &&
                    record.sha256 == QString::fromLatin1(sha256(bytes).toHex());
        } else {
            record.imagePath = imagePathFor(record.id);
            QFile image(record.imagePath);
            const QByteArray bytes = safeRegularFile(record.imagePath) && image.open(QIODevice::ReadOnly)
                                         ? image.readAll()
                                         : QByteArray();
            QImage decoded;
            valid = record.mime == QLatin1String("image/png") && !bytes.isEmpty() &&
                    bytes.size() <= maximumImageBytes && record.size == bytes.size() &&
                    record.sha256 == QString::fromLatin1(sha256(bytes).toHex()) &&
                    decoded.loadFromData(bytes) && decoded.width() == record.width &&
                    decoded.height() == record.height && record.width > 0 && record.height > 0 &&
                    qint64(record.width) * record.height <= maximumPixels;
        }
        if (valid) {
            ids.insert(record.id);
            m_records.append(record);
        }
    }

    if (m_records.size() > maximumRecords)
        m_records.resize(maximumRecords);
    m_loaded = true;
    cleanupOrphanImages();
    if (!saveManifest(m_records))
        return false;
    removeLegacyImagesDirectory();
    return true;
}

const QVector<ClipboardHistoryRecord> &ClipboardHistoryStore::records() const {
    return m_records;
}

bool ClipboardHistoryStore::persist(const QVector<ClipboardHistoryRecord> &records) {
    if (!saveManifest(records))
        return false;
    m_records = records;
    cleanupOrphanImages();
    return true;
}

ClipboardHistoryRecord ClipboardHistoryStore::addText(const QString &text, ClipboardRecordOrigin origin,
                                                       const QString &sourceDeviceId,
                                                       const QString &sourceDeviceName,
                                                       const QDateTime &created) {
    const QByteArray bytes = text.toUtf8();
    if ((!m_loaded && !load()) || text.isEmpty() || bytes.size() > maximumTextBytes)
        return {};

    ClipboardHistoryRecord record;
    record.origin = origin;
    record.kind = ClipboardRecordKind::Text;
    record.text = text;
    record.mime = QStringLiteral("text/plain");
    record.sha256 = QString::fromLatin1(sha256(bytes).toHex());
    record.size = bytes.size();
    record.sourceDeviceId = sourceDeviceId;
    record.sourceDeviceName = sourceDeviceName;
    record.created = created.isValid() ? created.toUTC() : QDateTime::currentDateTimeUtc();

    QVector<ClipboardHistoryRecord> updated = m_records;
    for (int i = 0; i < updated.size(); ++i) {
        if (!sameHistoryIdentity(updated.at(i), record))
            continue;
        ClipboardHistoryRecord duplicate = updated.takeAt(i);
        duplicate.created = record.created;
        updated.prepend(duplicate);
        return persist(updated) ? duplicate : ClipboardHistoryRecord();
    }

    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    updated.prepend(record);
    if (updated.size() > maximumRecords)
        updated.resize(maximumRecords);
    return persist(updated) ? record : ClipboardHistoryRecord();
}

ClipboardHistoryRecord ClipboardHistoryStore::addLocalText(const QString &text) {
    return addText(text, ClipboardRecordOrigin::Local, {}, {}, {});
}

ClipboardHistoryRecord ClipboardHistoryStore::addIncomingText(const QString &text,
                                                               const QString &sourceDeviceId,
                                                               const QString &sourceDeviceName,
                                                               const QDateTime &created) {
    return addText(text, ClipboardRecordOrigin::Incoming, sourceDeviceId, sourceDeviceName, created);
}

ClipboardHistoryRecord ClipboardHistoryStore::addImage(const QByteArray &encoded,
                                                        ClipboardRecordOrigin origin,
                                                        const QString &sourceDeviceId,
                                                        const QString &sourceDeviceName,
                                                        const QDateTime &created) {
    if ((!m_loaded && !load()) || encoded.isEmpty())
        return {};
    QImage image;
    if (!image.loadFromData(encoded) || qint64(image.width()) * image.height() > maximumPixels)
        return {};
    const QByteArray stored = encodePng(image);
    if (stored.isEmpty() || stored.size() > maximumImageBytes)
        return {};

    ClipboardHistoryRecord record;
    record.origin = origin;
    record.kind = ClipboardRecordKind::Image;
    record.mime = QStringLiteral("image/png");
    record.sha256 = QString::fromLatin1(sha256(stored).toHex());
    record.size = stored.size();
    record.width = image.width();
    record.height = image.height();
    record.sourceDeviceId = sourceDeviceId;
    record.sourceDeviceName = sourceDeviceName;
    record.created = created.isValid() ? created.toUTC() : QDateTime::currentDateTimeUtc();

    QVector<ClipboardHistoryRecord> updated = m_records;
    for (int i = 0; i < updated.size(); ++i) {
        if (!sameHistoryIdentity(updated.at(i), record))
            continue;
        ClipboardHistoryRecord duplicate = updated.takeAt(i);
        duplicate.created = record.created;
        updated.prepend(duplicate);
        return persist(updated) ? duplicate : ClipboardHistoryRecord();
    }

    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.imagePath = imagePathFor(record.id);
    if (!writeAtomically(record.imagePath, stored))
        return {};
    updated.prepend(record);
    if (updated.size() > maximumRecords)
        updated.resize(maximumRecords);
    if (persist(updated))
        return record;
    QFile::remove(record.imagePath);
    return {};
}

ClipboardHistoryRecord ClipboardHistoryStore::addLocalImage(const QByteArray &encoded, const QString &mime,
                                                             int width, int height) {
    if (!mime.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
        return {};
    QImage decoded;
    if (!decoded.loadFromData(encoded) || decoded.width() != width || decoded.height() != height)
        return {};
    return addImage(encoded, ClipboardRecordOrigin::Local, {}, {}, {});
}

ClipboardHistoryRecord ClipboardHistoryStore::addIncomingImageFile(
    const QString &sourcePath, const QString &mime, qint64 size, int width, int height,
    const QString &expectedSha256, const QString &sourceDeviceId, const QString &sourceDeviceName,
    const QDateTime &created) {
    if (!safeRegularFile(sourcePath) || !mime.startsWith(QLatin1String("image/"), Qt::CaseInsensitive) ||
        size < 0 || size > maximumImageBytes) {
        return {};
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = source.readAll();
    if (bytes.size() != size || QString::fromLatin1(sha256(bytes).toHex()).compare(
                                    expectedSha256, Qt::CaseInsensitive) != 0) {
        return {};
    }
    QImage decoded;
    if (!decoded.loadFromData(bytes) || decoded.width() != width || decoded.height() != height)
        return {};
    return addImage(bytes, ClipboardRecordOrigin::Incoming, sourceDeviceId, sourceDeviceName, created);
}

bool ClipboardHistoryStore::lookup(const QString &id, ClipboardHistoryRecord *record) const {
    for (const ClipboardHistoryRecord &known : m_records) {
        if (known.id == id) {
            if (record)
                *record = known;
            return true;
        }
    }
    return false;
}

bool ClipboardHistoryStore::remove(const QString &id) {
    return removeRecords({id});
}

bool ClipboardHistoryStore::removeIncomingRecords() {
    if ((!m_loaded && !load()))
        return false;
    QVector<ClipboardHistoryRecord> updated;
    updated.reserve(m_records.size());
    bool found = false;
    for (const ClipboardHistoryRecord &record : m_records) {
        if (record.origin == ClipboardRecordOrigin::Incoming) {
            found = true;
            continue;
        }
        updated.append(record);
    }
    return found && persist(updated);
}

bool ClipboardHistoryStore::removeRecords(const QStringList &ids) {
    if ((!m_loaded && !load()) || ids.isEmpty())
        return false;

    QSet<QString> removedIds;
    for (const QString &id : ids) {
        if (!id.isEmpty())
            removedIds.insert(id);
    }
    if (removedIds.isEmpty())
        return false;

    QVector<ClipboardHistoryRecord> updated;
    updated.reserve(m_records.size());
    bool found = false;
    for (const ClipboardHistoryRecord &record : m_records) {
        if (removedIds.contains(record.id)) {
            found = true;
            continue;
        }
        updated.append(record);
    }
    return found && persist(updated);
}
