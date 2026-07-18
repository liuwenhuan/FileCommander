#pragma once

#include <QSharedPointer>
#include <QString>
#include <QStringList>

#include "ArchiveNode.h"

// Thin facade over libarchive: build an in-memory tree of an archive's
// contents, and extract selected entries (or everything) to a directory.
// Supports whatever libarchive's "all formats" reader supports -- zip,
// tar, tar.gz, tar.bz2, tar.xz in practice.
class ArchiveHandler {
public:
    static bool isSupportedArchive(const QString &path);

    // Returns a null pointer on failure (see errorMessage).
    static QSharedPointer<ArchiveNode> buildTree(const QString &archivePath,
                                                  QString *errorMessage = nullptr);

    // entryFullPaths empty => extract everything. A directory entry in
    // the list extracts that directory and everything under it.
    static bool extract(const QString &archivePath, const QStringList &entryFullPaths,
                         const QString &destDir, QString *errorMessage = nullptr);
};
