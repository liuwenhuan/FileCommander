#pragma once

#include <functional>

#include <QString>

enum class ErrorAction {
    Skip,
    SkipAll,
    Retry,
    Overwrite,
    OverwriteAll,
    Rename,
    Cancel,
};

enum class OperationType {
    Copy,
    Move,
    Delete,
    Mkdir,
    Rename,
};

enum class OperationStatus {
    Pending,
    Running,
    Paused,
    Completed,
    Failed,
};

// The two files an overwrite prompt is about.
//
// The sizes travel with the paths because only the code that raised the
// conflict can obtain them. On a cross-provider transfer both files are on a
// server, so a QFileInfo over either path describes a same-named LOCAL file or
// nothing at all -- which is how the prompt came to offer "0 bytes" for a file
// that was 1.2 MB on the server, and why the sizes cannot simply be looked up
// again at the other end.
//
// -1 means "not known" (a directory, or a backend that could not say). The
// prompt must show that as unknown rather than as a number.
struct FileConflict {
    QString sourcePath;
    QString destPath;
    qint64 sourceSize = -1;
    qint64 destSize = -1;
};

// Called (on the GUI thread, via a blocking cross-thread invoke) whenever a
// copy/move would overwrite an existing destination file. Returning a
// *All or Cancel value short-circuits prompting for the rest of the batch.
using ConflictResolver = std::function<ErrorAction(const FileConflict &conflict)>;

// Called (on the GUI thread) when a copy/delete fails on a specific entry.
// Retry re-attempts, Skip/SkipAll continue past it, Cancel aborts the batch.
using ErrorResolver =
    std::function<ErrorAction(const QString &path, const QString &error)>;
