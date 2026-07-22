#pragma once

#include <QMutex>
#include <QString>
#include <QVector>

#include "FileProvider.h"

// A remote file backend over FTP (libcurl). Implements FileProvider so a
// FileSystemModel can browse an FTP server exactly as it browses the local
// filesystem or an SFTP host (see SftpProvider, which this mirrors
// structurally: a mutex-serialised session plus streaming read/write with
// resume).
//
// Control-plane calls (list/isDir/exists/rename/remove/mkdir) reuse a single
// curl "easy" handle (m_curl) so libcurl keeps the FTP control connection
// alive between requests instead of reconnecting every time. libcurl handles
// are not thread-safe and list() runs on a QtConcurrent worker thread, so
// every use of m_curl is serialised behind m_mutex, exactly like SftpProvider.
//
// Streaming reads/writes (openRead/openWrite/read/write) use a *separate*
// curl easy handle owned by the FileHandle and driven on a dedicated
// background thread inside curl_easy_perform(): curl's blocking easy
// interface transfers a whole request per call, so the background thread
// feeds/drains a small producer/consumer byte buffer, letting read()/write()
// hand back data one caller-paced chunk at a time as FileOperations::
// streamCopy() expects. Because each transfer gets its own connection, two
// concurrent transfers through the same provider instance (see
// OperationQueue's N-worker transfer pool) run on independent FTP data
// connections and never contend with each other or with list()/isDir().
class CurlFtpProvider : public FileProvider {
public:
    CurlFtpProvider();
    ~CurlFtpProvider() override;

    // Blocking connect: verifies host/port/credentials work. Returns true on
    // success; on failure returns false and writes a reason to *error (if
    // non-null), leaving the provider disconnected.
    bool connectToHost(const QString &host, int port, const QString &user,
                       const QString &password, QString *error);

    // Tears down the control connection. Safe to call when not connected.
    // Called automatically by the destructor.
    void disconnect();

    bool isConnected() const;
    QString host() const;

    // Connection label for network tabs: "user@host" (or "host" if no user).
    QString displayName() const override;

    // FileProvider overrides:
    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;   // POSIX normalisation
    QString parentPath(const QString &path) const override;  // POSIX parent; '/' -> ""
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;

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

    // Pure parsers exposed for unit testing without a live FTP server.
    static QVector<FileInfo> parseMlsdListing(const QByteArray &data, const QString &dirPath,
                                              bool showHidden);
    static QVector<FileInfo> parseUnixListing(const QByteArray &data, const QString &dirPath,
                                              bool showHidden);

private:
    // Builds the ftp:// URL for `path` (percent-encoded per segment).
    // isDirectory appends a trailing slash, which curl/most servers expect to
    // treat the URL as a listing rather than a file transfer.
    QString buildUrl(const QString &path, bool isDirectory) const;
    // Synchronous SIZE probe over m_curl; -1 if unknown / not a plain file.
    // Caller must hold m_mutex.
    qint64 remoteFileSizeLocked(const QString &path) const;
    // Runs one or more QUOTE (control) commands against the base URL with
    // NOBODY set; used by isDir/rename/remove/mkdir. Caller must hold
    // m_mutex. Returns the underlying CURLcode (as int, to keep <curl/curl.h>
    // out of this header).
    int runQuoteCommandsLocked(const QStringList &commands) const;

    mutable QMutex m_mutex;
    void *m_curl = nullptr; // CURL*; opaque here to avoid pulling curl.h into every includer
    char m_errorBuffer[256] = {};
    QString m_host;
    int m_port = 21;
    QString m_user;
    QString m_password;
    bool m_connected = false;
};
