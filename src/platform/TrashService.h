#pragma once

#include "PlatformResult.h"

#include <QStringList>
#include <memory>

class TrashService {
public:
    virtual ~TrashService() = default;
    // Successful moves may return opaque restore locations in PlatformResult::undoEntries.
    virtual PlatformResult moveToTrash(const QStringList &paths) = 0;
    virtual PlatformResult restoreFromTrash(const QStringList &entries) = 0;
};

std::unique_ptr<TrashService> createTrashService();
