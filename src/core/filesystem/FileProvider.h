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
    virtual ~FileHandle() = default;
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

    // Lists a directory's entries, excluding "." / "..". Worker-thread.
    virtual QVector<FileInfo> list(const QString &path, bool showHidden) const = 0;

    // Whether path is (or resolves to) a directory.
    virtual bool isDir(const QString &path) const = 0;

    // Normalised absolute form of path.
    virtual QString cleanPath(const QString &path) const = 0;

    // Parent directory of path, or an empty string if path is already a root.
    virtual QString parentPath(const QString &path) const = 0;

    // Concise human label for the connection this provider represents (e.g.
    // "user@host"), shown alongside the current directory on network tabs. The
    // default is empty: local/archive backends have no connection identity and
    // keep the plain directory-name tab label.
    virtual QString displayName() const { return {}; }

    // Short lowercase protocol identifier for network backends: "sftp", "smb",
    // "ftp", or "webdav". Used to pick the per-protocol tab icon so the user can
    // tell connection types apart at a glance. Empty for local/archive backends.
    virtual QString scheme() const { return {}; }

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
    virtual qint64 read(FileHandle * /*handle*/, char * /*buffer*/, qint64 /*maxSize*/) {
        return -1;
    }
    virtual qint64 write(FileHandle * /*handle*/, const char * /*buffer*/, qint64 /*size*/) {
        return -1;
    }
    virtual bool seek(FileHandle * /*handle*/, qint64 /*offset*/) { return false; }
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
