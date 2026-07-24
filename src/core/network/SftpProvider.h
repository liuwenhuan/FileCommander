#pragma once

#include <QMutex>
#include <QString>

#include "FileProvider.h"

// libssh2 forward decls to keep the C headers out of this interface.
struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

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

    // FileProvider overrides:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;   // POSIX normalisation
    QString parentPath(const QString &path) const override;  // POSIX parent; '/' -> ""
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;

    // Streaming I/O over the SFTP subsystem so cross-provider transfers can read
    // from / write to the remote host (with resume). Every call serialises on
    // m_mutex just like list()/isDir(), because the libssh2 session is shared
    // and not thread-safe and a directory scan may run concurrently.
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
    // Serialises every access to the session/sftp handles.
    mutable QMutex m_mutex;

    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_SFTP *m_sftp = nullptr;
    int m_socket = -1;
    QString m_host;
    int m_port = 22;
    QString m_user;
    QString m_password; // retained for reconnect()
    int m_timeoutMs = 12000;
};
