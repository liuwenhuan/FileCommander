#pragma once

#include <QString>

namespace PrivatePath {

// Applies the native private-storage policy. Unix uses owner-only mode bits;
// Windows uses a protected ACL instead of pretending POSIX bits are exact.
bool restrictDirectory(const QString &path);
bool restrictFile(const QString &path);

} // namespace PrivatePath
