#pragma once

#include "PlatformResult.h"

enum class PathFlavor { Posix, Windows };

class PathSemantics {
public:
    static bool isRoot(const QString &path, PathFlavor flavor);
    static QString parentPath(const QString &path, PathFlavor flavor);
    static bool equivalent(const QString &left, const QString &right, PathFlavor flavor);
    static PlatformResult validateComponent(const QString &component, PathFlavor flavor);
    static bool requiresCaseOnlyRename(const QString &from, const QString &to,
                                       PathFlavor flavor);
};
