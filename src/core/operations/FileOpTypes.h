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
