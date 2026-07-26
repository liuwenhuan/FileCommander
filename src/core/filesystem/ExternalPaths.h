#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "FileProvider.h" // RemoteLocation

class QMimeData;

// What leaves FileCommander when the user copies files to the clipboard or
// drags them out of a panel.
//
// A panel's path is its *backend's* path, not this machine's: on a network tab
// "/share/report.pdf" belongs to the server, and inside an archive
// "/etc/passwd" names an entry in the archive file. Handing either to another
// program as file:///share/report.pdf does not fail -- the receiver opens
// whatever LOCAL file happens to carry that name, or nothing at all. Both are
// silent, and the first one is the dangerous one.
//
// So the two payloads are separated here:
//   * the PUBLIC one (text/uri-list, x-special/gnome-copied-files) only ever
//     names something that really resolves outside this process -- see
//     externalUrlsFor();
//   * the PRIVATE one carries the backend paths verbatim, so FileCommander's own
//     paste/drop keeps working even when there is no public URL to give at all.
namespace fc {

// The private drag/clipboard format: line 0 is "cut" or "copy", then one
// backend path per line, verbatim (never a URL -- they are not file paths).
// Set on every copy/cut/drag regardless of backend, so an in-app paste or drop
// never has to reconstruct a path from the public payload.
constexpr char kInternalPathsMime[] = "application/x-filecommander-paths";

QByteArray encodeInternalPaths(const QStringList &paths, bool cut);
// Parses what encodeInternalPaths wrote. *cut (when non-null) receives the
// move/copy flag; it is left false for anything unparseable.
QStringList decodeInternalPaths(const QByteArray &data, bool *cut = nullptr);

// The backend paths a drop (or paste) is carrying: the private format above
// when it is there -- the only channel a remote or in-archive path survives --
// and otherwise the local file:// URLs another application supplied.
QStringList incomingPaths(const QMimeData *mime, bool *cut = nullptr);
// Whether `mime` looks like it carries files at all. Used by the drag-enter /
// drag-move handlers, which must also accept a drag whose public URL list is
// empty because its source was an archive or an unmounted server.
bool hasIncomingPaths(const QMimeData *mime);

// The one URL `path` may honestly be handed out as, or an empty QUrl when there
// is none. Pure, so the policy can be pinned down without a live connection:
//   `localFilesystem`   -- FileProvider::isLocalFilesystem()
//   `loc`               -- the backend's RemoteLocation (invalid for local and
//                          archive backends)
//   `mountedLocalPath`  -- the already-resolved gvfs path for `path`, or empty
//                          when the connection is not mounted right now
QUrl externalUrlFor(bool localFilesystem, const RemoteLocation &loc, const QString &path,
                    const QString &mountedLocalPath);

// The public URL list for `paths` on `provider`:
//   * local backend   -> file:// URLs, exactly as before;
//   * network backend -> the file's real path under the gvfs mount when the
//                        connection is already mounted (a file:// URL any
//                        program can open, straight off the server), otherwise
//                        the protocol URI (smb://user@host/share/...), which
//                        GIO/KIO-based programs resolve themselves and
//                        everything else refuses outright instead of opening
//                        the wrong file;
//   * archive backend -> nothing. An entry inside an archive has no name
//                        outside this process and no URI form to offer.
//
// Never mounts: both callers run on the GUI thread (a drag is inside a blocking
// exec loop), and `gio mount` can take seconds and prompt. An unmounted
// connection therefore degrades to the URI form rather than stalling.
//
// Entries with nothing to offer are dropped, so the result may be shorter than
// `paths` -- and empty, which is a valid answer meaning "tell the other
// application nothing".
QList<QUrl> externalUrlsFor(FileProvider *provider, const QStringList &paths);

// Writes both representations of `paths` into `mime`: the private format
// always, and the public URL list only when externalUrlsFor() had something
// honest to put in it. Shared by the clipboard (which layers its own GNOME
// cut/copy convention on top, reading the URLs back off `mime`) and by both
// views' drag payloads, so the two can never drift apart on what another
// application is allowed to see.
void setPathPayload(QMimeData *mime, FileProvider *provider, const QStringList &paths, bool cut);

} // namespace fc
