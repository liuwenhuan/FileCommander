#include "TrashService.h"

#import <AppKit/AppKit.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString tokenFor(const QString &original, const QString &trashed) {
    const QJsonObject object{
        {QStringLiteral("version"), 1},
        {QStringLiteral("original"), original},
        {QStringLiteral("trashed"), trashed},
    };
    return QString::fromLatin1(
        QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64());
}

bool decodeToken(const QString &token, QString *original, QString *trashed,
                QString *errorMessage) {
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromBase64(token.toLatin1()));
    if (!document.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The Trash restore token is invalid.");
        return false;
    }

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt() != 1 ||
        !object.value(QStringLiteral("original")).isString() ||
        !object.value(QStringLiteral("trashed")).isString()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The Trash restore token is invalid.");
        return false;
    }

    const QString originalPath = object.value(QStringLiteral("original")).toString();
    const QString trashedPath = object.value(QStringLiteral("trashed")).toString();
    if (!QDir::isAbsolutePath(originalPath) || !QDir::isAbsolutePath(trashedPath)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The Trash restore token contains a relative path.");
        return false;
    }

    if (original)
        *original = originalPath;
    if (trashed)
        *trashed = trashedPath;
    return true;
}

struct RecycleResult {
    bool ok = false;
    QString trashedPath;
    QString error;
};

RecycleResult recycleOne(const QString &path) {
    RecycleResult result;
    if (!QFileInfo(path).exists() && !QFileInfo(path).isSymLink()) {
        result.error = QStringLiteral("No such file: %1").arg(path);
        return result;
    }

    NSString *sourcePath =
        [NSString stringWithUTF8String:path.toUtf8().constData()];
    if (!sourcePath) {
        result.error = QStringLiteral("The source path is not valid UTF-8.");
        return result;
    }
    NSURL *sourceURL = [NSURL fileURLWithPath:sourcePath];

    NSURL *trashedURL = nil;
    NSError *operationError = nil;
    const BOOL recycled =
        [[NSFileManager defaultManager] trashItemAtURL:sourceURL
                                      resultingItemURL:&trashedURL
                                                 error:&operationError];
    if (!recycled) {
        if (operationError && operationError.localizedDescription)
            result.error = QString::fromUtf8(operationError.localizedDescription.UTF8String);
        else
            result.error = QStringLiteral("macOS could not move the item to the Trash.");
        return result;
    }

    if (!trashedURL) {
        result.error = QStringLiteral("macOS did not return the new Trash location.");
        return result;
    }
    result.ok = true;
    result.trashedPath = QString::fromUtf8(trashedURL.path.UTF8String);
    return result;
}

PlatformResult restoreOne(const QString &token) {
    QString original;
    QString trashed;
    QString error;
    if (!decodeToken(token, &original, &trashed, &error))
        return PlatformResult::failure(PlatformError::InvalidPath, error);
    if (!QFileInfo(trashed).exists() && !QFileInfo(trashed).isSymLink())
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("The item is no longer in the Trash."));
    if (QFileInfo(original).exists() || QFileInfo(original).isSymLink())
        return PlatformResult::failure(
            PlatformError::Busy,
            QStringLiteral("The original path already exists: %1").arg(original));
    if (!QDir().mkpath(QFileInfo(original).absolutePath()))
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            QStringLiteral("Could not create the original parent directory."));
    if (!QFile::rename(trashed, original))
        return PlatformResult::failure(
            PlatformError::NativeFailure,
            QStringLiteral("Could not restore the item from the Trash."));
    return PlatformResult::success();
}

class MacTrashService final : public TrashService {
public:
    PlatformResult moveToTrash(const QStringList &paths) override {
        QStringList undoEntries;
        for (const QString &path : paths) {
            const RecycleResult result = recycleOne(path);
            if (!result.ok)
                return PlatformResult::failure(PlatformError::NativeFailure, result.error);
            undoEntries.append(tokenFor(path, result.trashedPath));
        }
        return PlatformResult::success(undoEntries);
    }

    PlatformResult restoreFromTrash(const QStringList &entries) override {
        for (const QString &entry : entries) {
            const PlatformResult result = restoreOne(entry);
            if (!result.ok)
                return result;
        }
        return PlatformResult::success();
    }
};

} // namespace

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<MacTrashService>();
}
