#pragma once

#include <QString>

// Reads human-readable package metadata for Linux packages so the archive
// preview can show more than just the file tree:
//   * .deb -- the control file (Package/Version/Depends/Description, ...),
//     read from the nested control.tar.* member with libarchive.
//   * .rpm -- Name/Version/Summary/Description, parsed straight from the RPM
//     header (libarchive's rpm reader only exposes the payload, not the tags).
// Returns an empty string when the path is not a .deb/.rpm or the metadata
// could not be read; callers should hide the info panel in that case.
namespace PackageInfo {

QString forPackage(const QString &path);

} // namespace PackageInfo
