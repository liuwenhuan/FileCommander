#pragma once

#include "PlatformResult.h"

enum class PathFlavor { Posix, Windows };

class PathSemantics {
public:
    static bool isRoot(const QString &path, PathFlavor flavor);
    static QString parentPath(const QString &path, PathFlavor flavor);
    static bool equivalent(const QString &left, const QString &right, PathFlavor flavor);
    // Whether `path` is `ancestor` itself or lies underneath it.
    //
    // Copying or moving a directory into a place inside itself is the one
    // destination that cannot work: the copy writes into the tree it is still
    // reading, so it recurses into its own output and only stops when the path
    // grows too long for the filesystem. Nothing further down the stack notices
    // -- each individual copy is a perfectly legal one.
    //
    // Component-wise, not by string prefix: "/home/ann" is not inside
    // "/home/annex", though one is a prefix of the other.
    static bool isInsideOrSame(const QString &path, const QString &ancestor, PathFlavor flavor);
    static PlatformResult validateComponent(const QString &component, PathFlavor flavor);
    static bool requiresCaseOnlyRename(const QString &from, const QString &to,
                                       PathFlavor flavor);
};
