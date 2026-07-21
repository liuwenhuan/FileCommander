#pragma once

#include <QString>
#include <QVector>

#include "FileInfo.h"

// Abstract file backend behind a FileSystemModel. LocalFileProvider wraps the
// local filesystem (the current, unchanged behaviour); future backends (SFTP,
// ...) implement the same interface so the model, views, and operations don't
// hard-code local-filesystem calls.
//
// list() runs on a worker thread (the directory scan goes through
// QtConcurrent), so implementations must be safe to call off the GUI thread and
// must outlive any in-flight scan.
class FileProvider {
public:
    enum class RenameResult { Ok, AlreadyExists, Failed };

    virtual ~FileProvider() = default;

    // Lists a directory's entries, excluding "." / "..". Worker-thread.
    virtual QVector<FileInfo> list(const QString &path, bool showHidden) const = 0;

    // Whether path is (or resolves to) a directory.
    virtual bool isDir(const QString &path) const = 0;

    // Normalised absolute form of path.
    virtual QString cleanPath(const QString &path) const = 0;

    // Parent directory of path, or an empty string if path is already a root.
    virtual QString parentPath(const QString &path) const = 0;

    // Whether anything exists at path.
    virtual bool exists(const QString &path) const = 0;

    // Renames the entry at `path` to a sibling named `newName`. On success,
    // writes the resulting full path to *newPath (if non-null).
    virtual RenameResult rename(const QString &path, const QString &newName,
                                QString *newPath) = 0;
};
