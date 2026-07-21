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
};
