#pragma once

#include <QString>

struct RemoteLocation;

namespace fc::TextEncodingIdentity {

// Stable source identities for remembered manual text encodings. These name the
// file the user selected, not a GVfs mount or temporary extracted/downloaded
// copy handed to QFile-based viewers.
QString localPath(const QString &path);
QString remotePath(const RemoteLocation &location, const QString &providerPath);
QString archiveEntry(const QString &containerIdentity, const QString &entryPath);

} // namespace fc::TextEncodingIdentity
