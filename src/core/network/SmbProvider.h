#pragma once

#include <QMutex>
#include <QString>

#include "FileProvider.h"

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
    // to the share (with resume). Serialised on m_mutex like list().
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
    // Builds the "smb://host<path>" URL for a POSIX provider path.
    QString urlFor(const QString &path) const;

    // Serialises every access to the context/handles.
    mutable QMutex m_mutex;

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
};
