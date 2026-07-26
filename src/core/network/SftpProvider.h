#pragma once

#include <QMutex>
#include <QString>

#include "ConnectionPool.h"
#include "FileProvider.h"

// libssh2 forward decls to keep the C headers out of this interface.
struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

// One independent SSH+SFTP physical connection borrowed by the pool for a
// transfer. Defined in the .cpp (needs the libssh2 types); the pool only ever
// handles SftpConn* so an incomplete type is enough here.
struct SftpConn;

// A remote file backend over SFTP (libssh2). Implements the FileProvider
// interface so a FileSystemModel can browse a remote host exactly as it browses
// the local filesystem.
//
// Connection is blocking and set up once via connectToHost(); afterwards the
// session is used in blocking mode. A single libssh2 session is NOT thread-safe,
// and list() is invoked from a QtConcurrent worker thread, so every SFTP call
// (list/isDir/exists/rename) is serialised behind an internal mutex.
class SftpProvider : public FileProvider {
public:
    SftpProvider();
    ~SftpProvider() override;

    // Blocking connect: socket -> session handshake -> password auth -> sftp
    // init. Returns true on success; on failure returns false, writes a reason
    // to *error (if non-null), and leaves the provider disconnected.
    bool connectToHost(const QString &host, int port, const QString &user,
                       const QString &password, QString *error);

    // Tears down the sftp channel, session, and socket. Safe to call when not
    // connected. Called automatically by the destructor.
    void disconnect();

    bool isConnected() const;
    QString host() const;

    // Connection label for network tabs: "user@host" (or "host" if no user).
    QString displayName() const override;
    QString scheme() const override { return QStringLiteral("sftp"); }

    // Bounds the TCP connect phase and every subsequent blocking libssh2 call.
    // Must be set before connectToHost(). Ignored if <= 0.
    void setTimeoutMs(int ms) override { m_timeoutMs = ms > 0 ? ms : m_timeoutMs; }

    // Rebuilds the session from the credentials captured by the last successful
    // connectToHost(). Returns true on success.
    bool reconnect(QString *error) override;

    // Caps the transfer connection pool at `channels` independent SSH sessions,
    // matching the number of concurrent transfer workers. Injected by
    // NetworkSession from the configured maxConcurrentTransfers.
    void setMaxTransferChannels(int channels) override;

    // FileProvider overrides:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;   // POSIX normalisation
    QString parentPath(const QString &path) const override;  // POSIX parent; '/' -> ""
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;
    RenameResult moveTo(const QString &srcPath, const QString &dstPath) override;

    // Streaming I/O over the SFTP subsystem so cross-provider transfers can read
    // from / write to the remote host (with resume). openRead/openWrite borrow an
    // independent SSH connection from the pool and pin it in the returned handle;
    // read/write/seek then operate on that private connection with NO m_mutex, so
    // concurrent transfers run truly in parallel and never contend with the
    // interactive session's list()/isDir()/heartbeat. If the pool cannot hand out
    // a connection, it transparently falls back to the shared single session
    // (serialised on m_mutex like before), so a transfer always completes.
    FileHandle *openRead(const QString &path) override;
    FileHandle *openWrite(const QString &path, bool truncate) override;
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override;
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override;
    bool seek(FileHandle *handle, qint64 offset) override;
    qint64 handleSize(FileHandle *handle) override;
    void closeHandle(FileHandle *handle) override;
    bool canStream() const override { return true; }

    bool remove(const QString &path) override;
    bool mkdir(const QString &path) override;

private:
    // Builds one independent, authenticated SSH+SFTP connection (socket ->
    // handshake -> password/key auth -> sftp init). No lock, no member access
    // beyond the passed-in credentials; used both for the interactive session
    // (connectToHost) and for every pooled transfer connection (the Factory).
    // Returns a heap SftpConn* on success, or nullptr with *error populated.
    // authFailed (optional) is set to true when the failure was a credential
    // rejection specifically -- the interactive path passes it through to
    // FileProvider::lastConnectAuthFailed(); the pool Factory passes nullptr.
    static SftpConn *buildConnection(const QString &host, int port, const QString &user,
                                     const QString &password, int timeoutMs, QString *error,
                                     bool *authFailed = nullptr);
    // Fully tears down and frees a SftpConn (the pool Destroyer; self-contained,
    // captures no provider state so it is safe on the detached reaper thread).
    static void destroyConnection(SftpConn *conn);
    // Wires the pool's Factory/Destroyer/size once credentials are known.
    void configurePool();

    // Serialises every access to the shared interactive session/sftp handles
    // (list/isDir/rename/heartbeat and the fallback transfer path only).
    mutable QMutex m_mutex;

    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_SFTP *m_sftp = nullptr;
    int m_socket = -1;
    QString m_host;
    int m_port = 22;
    QString m_user;
    QString m_password; // retained for reconnect()
    int m_timeoutMs = 12000;

    // Pool of independent SSH connections for concurrent transfers.
    ConnectionPool<SftpConn> m_pool;
    int m_maxChannels = 2;
};
