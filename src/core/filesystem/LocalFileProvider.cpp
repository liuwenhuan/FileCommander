#include "LocalFileProvider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {
// Wraps a QFile so it can travel through the FileHandle interface. The QFile is
// opened by openRead/openWrite and closed/destroyed by closeHandle.
struct LocalHandle : FileHandle {
    QFile file;
    explicit LocalHandle(const QString &path) : file(path) {}
};
} // namespace

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

FileHandle *LocalFileProvider::openRead(const QString &path) {
    auto *h = new LocalHandle(path);
    if (!h->file.open(QIODevice::ReadOnly)) {
        delete h;
        return nullptr;
    }
    return h;
}

FileHandle *LocalFileProvider::openWrite(const QString &path, bool truncate) {
    auto *h = new LocalHandle(path);
    // truncate=false (resume): keep existing bytes, seek to append point later.
    QIODevice::OpenMode mode =
        QIODevice::WriteOnly | (truncate ? QIODevice::Truncate : QIODevice::Append);
    if (!h->file.open(mode)) {
        delete h;
        return nullptr;
    }
    return h;
}

qint64 LocalFileProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    auto *h = static_cast<LocalHandle *>(handle);
    return h ? h->file.read(buffer, maxSize) : -1;
}

qint64 LocalFileProvider::write(FileHandle *handle, const char *buffer, qint64 size) {
    auto *h = static_cast<LocalHandle *>(handle);
    return h ? h->file.write(buffer, size) : -1;
}

bool LocalFileProvider::seek(FileHandle *handle, qint64 offset) {
    auto *h = static_cast<LocalHandle *>(handle);
    return h && h->file.seek(offset);
}

qint64 LocalFileProvider::handleSize(FileHandle *handle) {
    auto *h = static_cast<LocalHandle *>(handle);
    return h ? h->file.size() : -1;
}

void LocalFileProvider::closeHandle(FileHandle *handle) {
    delete static_cast<LocalHandle *>(handle); // QFile closes in its destructor
}

bool LocalFileProvider::remove(const QString &path) {
    QFileInfo fi(path);
    if (fi.isDir())
        return QDir().rmdir(path);
    return QFile::remove(path);
}

bool LocalFileProvider::mkdir(const QString &path) {
    return QDir().mkpath(path);
}
