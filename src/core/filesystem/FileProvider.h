#pragma once

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
    enum class RenameResult { Ok, AlreadyExists, Failed };

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

    // Whether anything exists at path.
    virtual bool exists(const QString &path) const = 0;

    // Renames the entry at `path` to a sibling named `newName`. On success,
    // writes the resulting full path to *newPath (if non-null).
    virtual RenameResult rename(const QString &path, const QString &newName,
                                QString *newPath) = 0;

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

    // Removes a single file or empty dir (used by move = copy+remove). Providers
    // that support writes should implement it; default fails.
    virtual bool remove(const QString & /*path*/) { return false; }
    // Creates a directory (including parents where the backend allows). Default fails.
    virtual bool mkdir(const QString & /*path*/) { return false; }
};
