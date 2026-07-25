#pragma once

#include <QMutex>
#include <QString>

#include "ConnectionPool.h"
#include "FileProvider.h"
#include "SmbHelperClient.h"

// libsmbclient opaque context; forward-declared to keep the C header out of
// this interface (the .cpp includes <libsmbclient.h>). SMBCCTX is a typedef in
// <libsmbclient.h>; declaring the same typedef here is compatible with it.
struct _SMBCCTX;
typedef struct _SMBCCTX SMBCCTX;

// A remote file backend over SMB/CIFS (libsmbclient). Implements the
// FileProvider interface so a FileSystemModel can browse a Windows/Samba share
// exactly as it browses the local filesystem -- the native counterpart to
// SftpProvider/CurlFtpProvider, replacing the old GVfs mount for SMB.
//
// Path model: provider paths are POSIX-style and rooted at '/'. '/' lists the
// server's shares; '/share' lists a share's contents; '/share/sub/file' is a
// file. Internally each path maps to an "smb://<host><path>" URL. Credentials
// are supplied through libsmbclient's auth callback, never embedded in the URL.
//
// A single SMBCCTX is NOT thread-safe, and list() is invoked from a
// QtConcurrent worker thread, so every libsmbclient call is serialised behind
// an internal mutex (same discipline as SftpProvider).
class SmbProvider : public FileProvider {
public:
    SmbProvider();
    ~SmbProvider() override;

    // Blocking connect: creates and initialises an SMBCCTX with the given
    // credentials, then probes the server by listing its shares. Returns true
    // on success; on failure returns false, writes a reason to *error (if
    // non-null), and leaves the provider disconnected. `anonymous` connects as
    // guest (user/password ignored). `workgroup` may be empty (negotiated).
    bool connectToHost(const QString &host, const QString &user,
                       const QString &password, const QString &workgroup,
                       bool anonymous, QString *error);

    // Tears down the context. Safe to call when not connected. Called by dtor.
    void disconnect();

    bool isConnected() const;
    QString host() const;

    // Connection label for network tabs: "user@host" (or "host" if no user).
    QString displayName() const override;
    QString scheme() const override { return QStringLiteral("smb"); }

    // Bounds waits on connections and response data by ms (libsmbclient's
    // smbc_setTimeout). Must be set before connectToHost(). Ignored if <= 0.
    void setTimeoutMs(int ms) override { m_timeoutMs = ms > 0 ? ms : m_timeoutMs; }

    // Rebuilds the context from the credentials captured by the last successful
    // connectToHost(). Returns true on success.
    bool reconnect(QString *error) override;

    // Caps the transfer connection pool at `channels` independent SMBCCTX
    // contexts, matching the number of concurrent transfer workers. Injected by
    // NetworkSession from the configured maxConcurrentTransfers.
    void setMaxTransferChannels(int channels) override;

    // Read by the libsmbclient auth callback (a free function in the .cpp),
    // which runs synchronously inside a libsmbclient call this provider issued.
    // The values are set once in connectToHost() and never mutated after, so
    // the callback reads them without the lock.
    QString userForAuth() const;
    QString passwordForAuth() const;
    QString workgroupForAuth() const;

    // FileProvider overrides:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;   // POSIX normalisation
    QString parentPath(const QString &path) const override;  // POSIX parent; '/' -> ""
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;

    // Streaming I/O over SMB so cross-provider transfers can read from / write
    // to the share (with resume). openRead/openWrite borrow an independent
    // SMBCCTX from the pool and pin it in the returned handle; read/write/seek
    // then operate on that private context with NO m_mutex, so concurrent
    // transfers run in parallel and never contend with the interactive context's
    // list()/isDir()/heartbeat. If the pool cannot hand out a context, it falls
    // back to the shared interactive context (serialised on m_mutex).
    //
    // openRead has one further preference ahead of both: a helper subprocess
    // (see m_helpers). Since the in-process pool is capped at a single channel,
    // that is the only way two reads ever overlap. Reads only -- writes have no
    // helper path and are unchanged.
    FileHandle *openRead(const QString &path) override;
    FileHandle *openWrite(const QString &path, bool truncate) override;
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override;
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override;
    bool seek(FileHandle *handle, qint64 offset) override;
    qint64 handleSize(FileHandle *handle) override;
    void closeHandle(FileHandle *handle) override;
    bool canStream() const override { return true; }

    // The number of helper subprocesses that can serve reads in parallel, or 1
    // when no helper is available (the in-process path is strictly serial,
    // because libsmbclient cannot be driven concurrently). Lets a caller size
    // its worker pool to the parallelism that actually exists.
    int maxReadChannels() const override;

    bool setModifiedTime(const QString &path, const QDateTime &modified) override;

    bool remove(const QString &path) override;
    bool mkdir(const QString &path) override;

private:
    // Builds the "smb://host<path>" URL for a POSIX provider path.
    QString urlFor(const QString &path) const;

    // list()'s fast path: enumerates `url` with smbc_readdirplus2, which carries
    // each entry's stat in the directory response instead of costing a separate
    // round-trip per entry. Caller must hold m_mutex and have a live m_ctx.
    // Sets *supported false when the library returned nothing at all for this
    // directory (notably the server root, where only readdir lists the shares),
    // meaning the result is meaningless and the readdir path must be used.
    QVector<FileInfo> listPlus(const QString &dirPath, const QByteArray &url, bool showHidden,
                               bool *supported) const;

    // Creates, initialises and probes one independent SMBCCTX using the current
    // credentials (read via the auth callback, which reaches back through
    // smbc_getOptionUserData). No lock; used both for the interactive context
    // (connectToHost) and for every pooled transfer context (the Factory).
    // Returns the context on success, or nullptr with *error populated.
    // authFailed (optional) is set true when the server rejected the login
    // (EACCES/EPERM) -- passed through by the interactive connect to
    // FileProvider::lastConnectAuthFailed(); the pool Factory passes nullptr.
    SMBCCTX *buildContext(QString *error, bool *authFailed = nullptr);
    // Wires the pool's Factory/Destroyer/size once credentials are known.
    void configurePool();

    // Recomputes m_displayName ("user@host", or empty when disconnected) from
    // the current credentials. Call after any change to m_host/m_user, with
    // m_mutex held.
    void publishDisplayName();

    // Serialises every access to the shared interactive context/handles
    // (list/isDir/rename/heartbeat and the fallback transfer path only).
    mutable QMutex m_mutex;

    // Guards m_displayName ONLY. Deliberately separate from m_mutex: the GUI
    // thread calls displayName() to label a tab while the session thread may be
    // inside a multi-second list(), and m_mutex is held for that whole listing.
    // Sharing one mutex made the tab-label refresh in FilePanel::navigateTo()
    // freeze the GUI for the duration of the listing. This one is only ever held
    // across a string copy, never across network I/O. Lock order, where both are
    // taken: m_mutex first, then this.
    mutable QMutex m_identityMutex;
    QString m_displayName;

    SMBCCTX *m_ctx = nullptr;
    QString m_host;

    // Credentials, set once at connect and read (without the lock) by the
    // static auth callback via smbc_getOptionUserData(). Effectively const
    // after a successful connectToHost().
    QString m_user;
    QString m_password;
    QString m_workgroup;
    bool m_anonymous = false; // remembered so reconnect() reuses guest mode
    int m_timeoutMs = 12000;

    // Pool of SMBCCTX contexts. Capped at one on purpose: libsmbclient is not
    // thread-safe even across separate contexts -- its talloc pools and loaded
    // smb.conf are process-global, so two threads reading through two contexts
    // corrupt that state and abort inside the library ("Bad talloc magic
    // value"). Verified against a real server: two concurrent readers crash
    // within seconds, one never does. The pool stays in place for its lifetime
    // and hand-off machinery; it just never hands out a second channel.
    ConnectionPool<SMBCCTX> m_pool;
    int m_maxChannels = 1;

    // Out-of-process read channels, which are how this backend gets real
    // parallelism despite the single in-process channel above: each helper is a
    // separate process, so each has its own copy of libsmbclient's global
    // state. Used by openRead() for reads only; when no helper is available the
    // in-process path below runs unchanged, so this is pure acceleration.
    SmbHelperClient m_helpers;
};
