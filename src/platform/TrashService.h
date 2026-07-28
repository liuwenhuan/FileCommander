#pragma once

#include "PlatformResult.h"

#include <QStringList>
#include <memory>

class TrashService {
public:
    virtual ~TrashService() = default;
    virtual PlatformResult moveToTrash(const QStringList &paths) = 0;
};

std::unique_ptr<TrashService> createTrashService();
