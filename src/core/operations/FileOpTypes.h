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

// Called (on the GUI thread, via a blocking cross-thread invoke) whenever a
// copy/move would overwrite an existing destination file. Returning a
// *All or Cancel value short-circuits prompting for the rest of the batch.
using ConflictResolver =
    std::function<ErrorAction(const QString &source, const QString &destination)>;

// Called (on the GUI thread) when a copy/delete fails on a specific entry.
// Retry re-attempts, Skip/SkipAll continue past it, Cancel aborts the batch.
using ErrorResolver =
    std::function<ErrorAction(const QString &path, const QString &error)>;
