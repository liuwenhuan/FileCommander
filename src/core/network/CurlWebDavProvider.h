#pragma once

#include <QMutex>
#include <QString>
#include <QVector>

#include "FileProvider.h"

// A remote file backend over WebDAV (libcurl, HTTP or HTTPS). Implements
// FileProvider so a FileSystemModel can browse a WebDAV share exactly as it
// browses the local filesystem or an SFTP/FTP host (mirrors SftpProvider's
// structure: a mutex-serialised session plus streaming read/write).
//
// Control-plane calls (list via PROPFIND, isDir/exists via PROPFIND Depth:0,
// rename via MOVE, remove via DELETE, mkdir via MKCOL) reuse a single curl
// "easy" handle (m_curl) so libcurl keeps the HTTP connection alive between
// requests. libcurl handles are not thread-safe and list() runs on a
// QtConcurrent worker thread, so every use of m_curl is serialised behind
// m_mutex.
//
// Streaming reads/writes use a *separate* curl easy handle owned by the
// FileHandle and driven on a dedicated background thread inside
// curl_easy_perform() (GET with Range for download, PUT for upload), for the
// same reason as CurlFtpProvider: curl's blocking easy interface runs a whole
// request per call, so a producer/consumer pipe lets read()/write() hand
// back data one caller-paced chunk at a time.
//
// WebDAV has no standardised way to resume an interrupted PUT upload (unlike
// FTP's APPE/REST or HTTP GET's Range header), so seek() on a write handle
// refuses any nonzero offset: FileOperations::streamCopy() then reports a
// clean, retryable failure instead of silently uploading only the tail of a
// file and corrupting the remote copy. Resumed *downloads* are fully
// supported via the Range header.
class CurlWebDavProvider : public FileProvider {
public:
    CurlWebDavProvider();
    ~CurlWebDavProvider() override;

    // Blocking connect: verifies host/port/credentials work. Returns true on
    // success; on failure returns false and writes a reason to *error (if
    // non-null), leaving the provider disconnected.
    bool connectToHost(const QString &host, int port, const QString &user,
                       const QString &password, bool useHttps, QString *error);

    void disconnect();
    bool isConnected() const;
    QString host() const;

    // Connection label for network tabs: "user@host" (or "host" if no user).
    QString displayName() const override;
    QString scheme() const override { return QStringLiteral("webdav"); }

    // Bounds the connect phase (and control-plane requests) by ms. Must be set
    // before connectToHost(). Ignored if <= 0.
    void setTimeoutMs(int ms) override { m_timeoutMs = ms > 0 ? ms : m_timeoutMs; }

    // Rebuilds the connection from the credentials captured by the last
    // successful connectToHost(). Returns true on success.
    bool reconnect(QString *error) override;

    // FileProvider overrides:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;
    QString parentPath(const QString &path) const override;
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;
    RenameResult moveTo(const QString &srcPath, const QString &dstPath) override;

    FileHandle *openRead(const QString &path) override;
    FileHandle *openWrite(const QString &path, bool truncate) override;
    // A size >= 0 is sent as Content-Length, so the PUT is not chunked -- see
    // the CURLOPT_INFILESIZE_LARGE comment in the .cpp for why that matters
    // when there is an HTTP proxy in the path.
    void setExpectedWriteSize(FileHandle *handle, qint64 totalSize) override;
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override;
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override;
    bool seek(FileHandle *handle, qint64 offset) override;
    qint64 handleSize(FileHandle *handle) override;
    void closeHandle(FileHandle *handle) override;
    bool closeHandleStatus(FileHandle *handle) override;
    bool canStream() const override { return true; }

    // WebDAV is HTTP, so reads genuinely run side by side: openRead() takes the
    // provider lock only long enough to copy the connection settings, and every
    // handle then owns a separate curl easy handle guarded by its own mutex.
    // Nothing is shared between two readers, which is what separates this from
    // SMB -- libsmbclient cannot be driven concurrently at all and needs helper
    // subprocesses to get past one channel.
    //
    // Four, from the throughput measured against a real server (12 files):
    //
    //     concurrency   whole files      256 KB ranges
    //         1           13.1 MB/s          0.31 s
    //         2           18.9 MB/s          0.20 s
    //         4           23.1 MB/s          0.14 s
    //         8           26.7 MB/s          0.13 s
    //
    // Range requests are the shape thumbnailing actually uses, and they flatten
    // out at four -- eight buys 7% more for twice the sockets, twice the memory
    // held in flight, and a deeper queue for the scroll-preemption to work
    // against. Sixteen concurrent requests drew no throttling (no 503s), so the
    // limit here is chosen for diminishing returns rather than server tolerance.
    int maxReadChannels() const override { return 4; }

    bool remove(const QString &path) override;
    bool mkdir(const QString &path) override;

    // Pure parser exposed for unit testing without a live WebDAV server.
    // `basePath` is excluded from the results (PROPFIND Depth:1 includes the
    // collection itself as the first <response> entry); pass a value that can
    // never match a real href (e.g. a null QString) to keep every entry, as
    // used internally for a Depth:0 single-entry stat.
    static QVector<FileInfo> parsePropfindXml(const QByteArray &data, const QString &basePath,
                                              bool showHidden);

private:
    QString buildUrl(const QString &path, bool isDirectory) const;
    // Runs a PROPFIND Depth:0 against `path` and reports whether it exists /
    // is a collection. Caller must hold m_mutex.
    bool statEntryLocked(const QString &path, bool *isDirOut) const;
    qint64 remoteFileSizeLocked(const QString &path) const;

    mutable QMutex m_mutex;
    void *m_curl = nullptr; // CURL*; opaque here to avoid pulling curl.h into every includer
    char m_errorBuffer[256] = {};
    QString m_host;
    int m_port = 80;
    QString m_user;
    QString m_password;
    bool m_useHttps = false;
    int m_timeoutMs = 12000;
    bool m_connected = false;
};
