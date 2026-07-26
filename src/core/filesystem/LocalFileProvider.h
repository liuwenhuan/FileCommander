#pragma once

#include "FileProvider.h"

// The local-filesystem backend: the default provider, wrapping the QDir/
// QFileInfo logic the model used directly before the VFS abstraction. Stateless
// and thread-safe, so a single shared instance() is fine.
class LocalFileProvider : public FileProvider {
public:
    static LocalFileProvider *instance();

    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;
    QString parentPath(const QString &path) const override;
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;

    // The one backend whose paths really are local-filesystem paths.
    bool isLocalFilesystem() const override { return true; }

    // Streaming I/O (backed by QFile) so cross-provider transfers can read from
    // or write to the local filesystem.
    FileHandle *openRead(const QString &path) override;
    FileHandle *openWrite(const QString &path, bool truncate) override;
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override;
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override;
    bool seek(FileHandle *handle, qint64 offset) override;
    qint64 handleSize(FileHandle *handle) override;
    void closeHandle(FileHandle *handle) override;
    bool canStream() const override { return true; }

    bool setModifiedTime(const QString &path, const QDateTime &modified) override;

    bool remove(const QString &path) override;
    bool mkdir(const QString &path) override;
};
