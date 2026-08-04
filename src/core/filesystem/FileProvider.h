#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include "FileInfo.h"

// Abstract file backend behind a FileSystemModel. LocalFileProvider wraps the
// local filesystem (the current, unchanged behaviour); future backends (SFTP,
// ...) implement the same interface so the model, views, and operations don't
// hard-code local-filesystem calls.
//
// list() runs on a worker thread (the directory scan goes through
// QtConcurrent), so implementations must be safe to call off the GUI thread and
// must outlive any in-flight scan.
// Opaque streaming file handle. Each provider returns its own subclass from
// openRead/openWrite and operates on it via read/write/seek/closeHandle.
class FileHandle {
public:
    enum class StreamError { None, NoSpace, PermissionDenied, ConnectionLost, Other };

    virtual ~FileHandle() = default;
    virtual StreamError streamError() const { return StreamError::None; }
    virtual QString streamErrorDetail() const { return {}; }
};

struct CloseHandleResult {
    bool committed = true;
    FileHandle::StreamError error = FileHandle::StreamError::None;
    QString detail;
};

// Everything needed to name this backend's server to an *external*, URI-based
// tool -- in practice gvfs, which is how a remote file gets a real path on this
// machine that other programs and libraries can open by name.
//
// Deliberately structured rather than one URI string, because a gvfs mount spec
// scatters the pieces: SMB's share becomes part of the mount's identity while
// SFTP's path does not, the port only appears when it is non-default, and the
// username sits in its own field. displayName() ("user@host") has already thrown
// away enough of that to be unusable here, which is why this exists separately.
struct RemoteLocation {
    // The GVFS URI scheme, which is NOT FileProvider::scheme(): WebDAV splits
    // into "dav" and "davs" by transport, a distinction "webdav" cannot carry.
    QString scheme; // "sftp" | "smb" | "ftp" | "dav" | "davs"
    QString host;
    int port = 0;   // 0 = protocol default; then omitted from the URI entirely
    QString user;   // empty = anonymous, i.e. no userinfo in the URI

    // The login password, present for one purpose only: answering an on-demand
    // `gio mount` so the user is not asked for a password they already gave us.
    // Never log it, never persist it, never put it in a URI (gvfs records mount
    // URIs in its own state). Backends already hold it in memory for reconnect(),
    // so surfacing it here adds no new exposure.
    QString password;

    // True when the connection has a login the mount can be answered with;
    // false means an anonymous/guest connection, which `gio mount -a` handles.
    bool anonymous = false;

    bool isValid() const { return !scheme.isEmpty() && !host.isEmpty(); }
};

class FileProvider {
public:
    // Unsupported is distinct from Failed on purpose: it means "this backend
    // cannot express this operation, ask someone else", not "the operation was
    // attempted and went wrong". Only moveTo() ever returns it -- rename() is
    // implemented by every backend and never does. Callers that treat the two
    // alike would turn a routine fall-back into a user-visible error.
    enum class RenameResult { Ok, AlreadyExists, Failed, Unsupported };

    virtual ~FileProvider() = default;

    // How the most recent list() call ended. list() returns a plain vector, and
    // an empty one is indistinguishable from a directory that really is empty --
    // which is how a share the server refused to enumerate came to be shown as
    // an empty folder with no explanation at all.
    //
    // AccessDenied is separate from Failed because the two want different
    // recoveries: a denial is answerable by credentials, so the session raises a
    // login prompt and re-lists once the user has typed them, while a plain
    // failure is only worth reporting.
    enum class ListStatus { Ok, Failed, AccessDenied };

    // Result of the last list() on THIS provider. Read on the same thread that
    // made the call, immediately after it returns (NetworkSession does both on
    // its worker thread), so no synchronisation beyond that ordering is implied.
    // The default never reports a failure, which keeps every backend that has
    // not been taught this behaving exactly as before.
    virtual ListStatus lastListStatus() const { return ListStatus::Ok; }
    // Human-readable reason for a non-Ok lastListStatus (may be empty).
    virtual QString lastListError() const { return {}; }

    // Lists a directory's entries, excluding "." / "..". Worker-thread.
    // Implementations that can distinguish failure from emptiness must record it
    // for lastListStatus() -- returning an empty vector is a claim that the
    // directory is empty.
    virtual QVector<FileInfo> list(const QString &path, bool showHidden) const = 0;

    // Whether path is (or resolves to) a directory.
    virtual bool isDir(const QString &path) const = 0;

    // Normalised absolute form of path.
    virtual QString cleanPath(const QString &path) const = 0;

    // Parent directory of path, or an empty string if path is already a root.
    virtual QString parentPath(const QString &path) const = 0;

    // Whether this backend's paths ARE paths on this machine's filesystem, so
    // handing one straight to QFile/QDir/QFileInfo operates on the very same
    // entry the user is looking at. Only the local backend can say yes.
    //
    // Network backends use the server's paths and archive backends use their own
    // in-archive paths (rooted at "/", so an archive holding an "etc/passwd"
    // entry yields the path "/etc/passwd"). Passing either to QFile silently
    // opens whatever LOCAL file happens to share that name -- it does not fail,
    // which is what makes it dangerous rather than merely wrong. Any code that
    // is about to bypass the provider and touch the path directly must ask this
    // first.
    //
    // Deliberately NOT expressed as displayName().isEmpty(): that test is how
    // network tabs are recognised elsewhere, but ArchiveProvider has no
    // displayName either, so it reads as "local" there. The default here is
    // false so a backend added later is refused until it opts in.
    virtual bool isLocalFilesystem() const { return false; }

    // Whether the link this backend is using was already open before it asked --
    // an OS-level session another application (or an earlier mapping)
    // established, which this backend adopted rather than replaced.
    //
    // It matters to the user because the adopted session carries somebody else's
    // identity: Windows refuses a second session to one server under a different
    // user name, so asking for credentials there would request a password it
    // will not apply. Reusing the session is the workable answer, but browsing
    // as an identity you did not choose should not happen silently.
    //
    // Reported as two pieces rather than a finished sentence so the wording (and
    // its translation) stays in the UI layer with every other user-facing string.
    virtual bool reusedExistingSession() const { return false; }
    // Who owns that adopted session, or empty when it cannot be determined.
    virtual QString reusedSessionUser() const { return {}; }

    // Whether this backend's listing is synthetic -- rows that stand for places
    // rather than for files in a directory (the computer view's drives, saved
    // servers and network hosts). The model uses it to decide whether to ask the
    // three hooks below at all, so ordinary backends pay one bool for a feature
    // they don't use, and none of them can accidentally be asked to describe a
    // row it never produced.
    virtual bool isVirtualListing() const { return false; }

    // What the Type column should say for `path`, e.g. "Server". Empty means
    // "no opinion" and the model falls back to deriving a type from the entry
    // itself, which is the right answer for every real file.
    virtual QString entryTypeLabel(const QString & /*path*/) const { return {}; }

    // What the Size column should say for `path`. Empty means "no opinion" and
    // the model falls back to its usual formatting -- which for a synthetic row
    // is "<DIR>", true but useless for a drive, where the interesting number is
    // how full it is.
    virtual QString entrySizeText(const QString & /*path*/) const { return {}; }

    // Resource path of the icon for `path` (":/icons/dev-smb.svg"). Empty means
    // the usual icon resolution applies -- which is what a synthetic *folder*
    // row wants, so it keeps the same folder icon as everywhere else.
    virtual QString entryIconPath(const QString & /*path*/) const { return {}; }

    // Whether a row in a synthetic listing can be renamed at all, and what the
    // editor should start from -- which is NOT the display name. A drive reads
    // "Windows (C:)" and only the "Windows" half is the user's to change: the
    // letter is the volume's identity, and letting it be typed over would offer
    // a rename that cannot mean anything. Seeding the editor with the label
    // alone is what makes the letter unreachable, rather than validating it
    // back out afterwards.
    //
    // Default false, so a synthetic backend is read-only until it says
    // otherwise -- a saved server, a bookmark and a discovered host all have
    // names that are not the system's to change. An empty seed is valid (an
    // unlabelled volume), which is why the two are separate questions.
    virtual bool entryIsRenameable(const QString & /*path*/) const { return false; }
    virtual QString entryRenameSeed(const QString & /*path*/) const { return {}; }

    // A REAL path whose system icon should be preferred over entryIconPath's
    // artwork -- a drive root, so the row shows the icon the platform gives
    // that particular volume rather than a generic disk glyph. Empty (the
    // default, and the answer whenever the platform has no such icon) means
    // entryIconPath decides, so a backend that does not care is already right.
    virtual QString entrySystemIconPath(const QString & /*path*/) const { return {}; }

    // Section ordinal for `path` within a synthetic listing. Rows sort by this
    // first, so the drives stay above the user folders and the servers below
    // them however the user then sorts the columns. Meaningless (and never
    // consulted) unless isVirtualListing() is true.
    virtual int entrySortGroup(const QString & /*path*/) const { return 0; }

    // Concise human label for the connection this provider represents (e.g.
    // "user@host"), shown alongside the current directory on network tabs. The
    // default is empty: local/archive backends have no connection identity and
    // keep the plain directory-name tab label.
    virtual QString displayName() const { return {}; }

    // Short lowercase protocol identifier for network backends: "sftp", "smb",
    // "ftp", or "webdav". Used to pick the per-protocol tab icon so the user can
    // tell connection types apart at a glance. Empty for local/archive backends.
    virtual QString scheme() const { return {}; }

    // How to name this backend's server to gvfs, so a caller that genuinely
    // needs a real local path (handing a file to an external program, or to a
    // library that opens by filename) can get one -- see GvfsMounter::
    // localPathFor(). Invalid by default: only a live network connection has a
    // server to name, and a backend that hasn't been taught this stays refused
    // rather than yielding a half-built URI.
    //
    // Directory browsing must NOT go through this. gvfs lists a directory with
    // one getattr per entry over FUSE, which is an order of magnitude slower
    // than these backends' own list() -- measured at 139.7 ms per entry over
    // SFTP on a 120 ms link. This is for single files that need a name on disk.
    virtual RemoteLocation remoteLocation() const { return {}; }

    // Path that the Windows Shell can open for thumbnail extraction. Local
    // files are already named directly by their path, so the default is empty
    // and only network backends with a native Windows path override it.
    virtual QString shellAccessiblePath(const QString & /*path*/) const { return {}; }

    // Sets the connect/operation timeout (milliseconds) for network backends, so
    // a stalled connection or request fails instead of hanging indefinitely.
    // Local and archive backends ignore it. Must be set before connectToHost to
    // bound the connection phase. NetworkSession injects the configured value.
    virtual void setTimeoutMs(int /*ms*/) {}

    // Re-establishes the connection using the credentials captured by the last
    // successful connectToHost, for transparent reconnection after a drop.
    // Returns true on success. Network backends override; the default fails
    // (local/archive have no connection to re-establish).
    virtual bool reconnect(QString * /*error*/) { return false; }

    // Whether the most recent interactive connect (connectToHost/reconnect)
    // failed specifically because the server rejected the credentials -- i.e. the
    // right recovery is to prompt for a username/password, not to keep retrying
    // the same (anonymous) dial. Network backends set this from their real error
    // code (SFTP: userauth failure; SMB: EACCES/EPERM; FTP: CURLE_LOGIN_DENIED;
    // WebDAV: HTTP 401) rather than fragile message-string matching. Read on the
    // same worker thread right after the connect returns; the transfer pool's own
    // (background) connection builds never touch it.
    bool lastConnectAuthFailed() const { return m_lastConnectAuthFailed; }

    // Whether anything exists at path.
    virtual bool exists(const QString &path) const = 0;

    // Renames the entry at `path` to a sibling named `newName`. On success,
    // writes the resulting full path to *newPath (if non-null).
    virtual RenameResult rename(const QString &path, const QString &newName,
                                QString *newPath) = 0;

    // Moves `srcPath` to `dstPath` *within this same backend*, entirely on the
    // server. Unlike rename() above, dstPath is a full path, so this can express
    // a move into a different directory -- the case that otherwise has to drag
    // every byte down to the client and back up again.
    //
    // Measured against a real SMB server, an 8 MB same-share cross-directory
    // move takes 0.02s this way versus 3.8s streamed through the client (191x);
    // the gap grows with file size, and a directory tree moves in one call.
    //
    // This is a *fast path*, not a replacement. The contract is deliberately
    // permissive so callers can always retreat to streaming:
    //   Ok            -- moved; the source no longer exists.
    //   AlreadyExists -- something occupies dstPath; nothing was moved. The
    //                    caller should run its normal conflict resolution.
    //   Unsupported   -- this backend/server can't do it (a different SMB share,
    //                    a different filesystem behind SFTP, a backend with no
    //                    server-side move at all). Nothing was moved and nothing
    //                    is wrong: fall back to copy+delete silently.
    //   Failed        -- attempted and genuinely failed. Callers still fall back
    //                    rather than surfacing this, because a backend cannot
    //                    always tell "can't" from "didn't" (SFTP reports both a
    //                    cross-filesystem move and an occupied destination as
    //                    SSH_FX_FAILURE). Implementations MUST guarantee that a
    //                    non-Ok return left the source intact.
    //
    // The default refuses, so a backend that hasn't been verified against a real
    // server keeps the existing streaming behaviour.
    virtual RenameResult moveTo(const QString & /*srcPath*/, const QString & /*dstPath*/) {
        return RenameResult::Unsupported;
    }

    // --- Streaming I/O for cross-provider transfers (copy/move between a local
    // and a remote provider, with resume). Providers that can't stream leave the
    // defaults (openRead/openWrite return nullptr → transfer unsupported). The
    // caller owns the returned handle and must pass it to closeHandle().
    virtual FileHandle *openRead(const QString & /*path*/) { return nullptr; }
    // truncate=false opens/creates for resume (append at seek position).
    virtual FileHandle *openWrite(const QString & /*path*/, bool /*truncate*/) { return nullptr; }

    // Announces how many bytes are about to be written to an open write handle,
    // or -1 when the caller genuinely cannot know. Must be called after
    // openWrite() and BEFORE the first write() -- a backend may have to commit
    // to a framing decision the moment the first byte moves.
    //
    // A hint, not a contract: backends may ignore it, and a wrong value must
    // never corrupt data. It exists for streamed HTTP PUT, which has to choose
    // between declaring Content-Length and sending Transfer-Encoding: chunked
    // before the body starts, and chunked is refused outright by some proxies
    // and middleboxes (see CurlWebDavProvider). Backends writing over a plain
    // file descriptor (local/SFTP/SMB/FTP) have no use for it and keep the
    // default no-op, which is why this is a separate call rather than an extra
    // openWrite() parameter every provider would have to thread through.
    virtual void setExpectedWriteSize(FileHandle * /*handle*/, qint64 /*totalSize*/) {}
    virtual qint64 read(FileHandle * /*handle*/, char * /*buffer*/, qint64 /*maxSize*/) {
        return -1;
    }
    virtual qint64 write(FileHandle * /*handle*/, const char * /*buffer*/, qint64 /*size*/) {
        return -1;
    }
    virtual bool seek(FileHandle * /*handle*/, qint64 /*offset*/) { return false; }
    // Whether an existing partial destination may be continued by seeking an
    // open write handle. WebDAV supports ranged reads but has no standard PUT
    // resume, so it overrides this while local, SMB, SFTP, and FTP keep true.
    virtual bool supportsWriteResume() const { return true; }
    // Size in bytes of an open handle's file, or -1 if unknown (used for resume).
    virtual qint64 handleSize(FileHandle * /*handle*/) { return -1; }
    virtual void closeHandle(FileHandle *handle) { delete handle; }
    // Like closeHandle, but reports whether the file was fully and successfully
    // committed. Streamed uploads (FTP/WebDAV) only learn the real server-side
    // result when the transfer thread finishes at close time, so write() can
    // return success for bytes that never actually land; a transfer MUST check
    // this instead of assuming success. The default forwards to closeHandle and
    // reports success -- synchronous backends already surface write failures
    // through write()'s return value.
    virtual bool closeHandleStatus(FileHandle *handle) {
        closeHandle(handle);
        return true;
    }
    // Closes a stream while retaining any backend-specific failure detected at
    // final commit time. The default preserves the existing boolean contract.
    virtual CloseHandleResult closeHandleResult(FileHandle *handle) {
        return {closeHandleStatus(handle)};
    }

    // Capability probe: whether this provider supports the streaming I/O above.
    virtual bool canStream() const { return false; }

    // How many reads this backend can genuinely serve at the same time, i.e.
    // how many independent read channels openRead() can hand out before they
    // start queueing behind one another. One means strictly serial.
    //
    // Callers that size a worker pool should ask rather than assume: extra
    // workers on a single-channel backend only deepen the queue and hold more
    // memory. SMB is the case that matters -- libsmbclient cannot be driven
    // concurrently in-process, so it reports 1 until its helper subprocesses
    // are confirmed available, and only then reports the real number.
    virtual int maxReadChannels() const { return 1; }

    // Stamps `path` with `modified` as its modification time, so a copy carries
    // the source's timestamp instead of being dated to the moment it was written.
    //
    // Best-effort by contract: a false return means "this backend can't set
    // times", NOT that the copy failed. Callers must never fail a transfer over
    // it -- the file is already written, and the worst case is the pre-existing
    // behaviour of a fresh timestamp.
    //
    // Only backends verified against a real server implement this. SFTP, FTP and
    // WebDAV keep the default: their protocols do expose a way to do it
    // (SETSTAT / MFMT / PROPPATCH), but none of it has been tried against an
    // actual server here, and shipping an unverified write path is worse than
    // leaving the old behaviour in place.
    virtual bool setModifiedTime(const QString & /*path*/, const QDateTime & /*modified*/) {
        return false;
    }

    // Removes a single file or empty dir (used by move = copy+remove). Providers
    // that support writes should implement it; default fails.
    virtual bool remove(const QString & /*path*/) { return false; }
    // Creates a directory (including parents where the backend allows). Default fails.
    virtual bool mkdir(const QString & /*path*/) { return false; }

    // Sets the maximum number of independent physical connections this provider
    // may open for concurrent streaming transfers (its internal connection
    // pool's cap). Backends without a pool (local/archive, and the FTP/WebDAV
    // backends whose per-handle curl handle already carries its own connection)
    // ignore it. NetworkSession injects the configured maxConcurrentTransfers so
    // the pool never opens more connections than there are transfer workers.
    virtual void setMaxTransferChannels(int /*channels*/) {}

protected:
    // Set by network backends' connectToHost/reconnect when the failure was an
    // authentication rejection (see lastConnectAuthFailed()). Non-network
    // providers never touch it, so it stays false.
    bool m_lastConnectAuthFailed = false;
};
