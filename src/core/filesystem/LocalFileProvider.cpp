#include "LocalFileProvider.h"

#include <QDir>
#include <QFileInfo>

LocalFileProvider *LocalFileProvider::instance() {
    static LocalFileProvider provider;
    return &provider;
}

QVector<FileInfo> LocalFileProvider::list(const QString &path, bool showHidden) const {
    QVector<FileInfo> result;
    QDir dir(path);
    QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
    if (showHidden)
        filters |= QDir::Hidden;

    const QFileInfoList entries = dir.entryInfoList(filters);
    result.reserve(entries.size());
    for (const QFileInfo &qfi : entries)
        result.append(FileInfo(qfi.absoluteFilePath()));
    return result;
}

bool LocalFileProvider::isDir(const QString &path) const {
    return QFileInfo(path).isDir();
}

QString LocalFileProvider::cleanPath(const QString &path) const {
    return QDir(path).absolutePath();
}

QString LocalFileProvider::parentPath(const QString &path) const {
    QDir dir(QDir::cleanPath(path));
    if (dir.isRoot() || !dir.cdUp())
        return QString(); // already at a filesystem root
    return dir.absolutePath();
}

bool LocalFileProvider::exists(const QString &path) const {
    return QFileInfo::exists(path);
}

FileProvider::RenameResult LocalFileProvider::rename(const QString &path, const QString &newName,
                                                     QString *newPath) {
    const QFileInfo fi(path);
    const QString destPath = fi.dir().filePath(newName);
    if (QFileInfo::exists(destPath))
        return RenameResult::AlreadyExists;
    if (!QDir().rename(path, destPath))
        return RenameResult::Failed;
    if (newPath)
        *newPath = destPath;
    return RenameResult::Ok;
}
