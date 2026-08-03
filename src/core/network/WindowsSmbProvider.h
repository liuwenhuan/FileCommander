#pragma once

#include "FileProvider.h"
#include "WindowsSmbSession.h"

#include <QString>

class WindowsSmbProvider : public FileProvider {
public:
    WindowsSmbProvider() = default;

    bool connectToHost(const QString &host, const QString &user,
                       const QString &password, const QString &workgroup,
                       bool anonymous, QString *error);
    void disconnect();
    // A live session, not merely a remembered host name. The old test (host
    // string non-empty) was true the instant a name was typed, which is what let
    // a never-established connection report itself as connected.
    bool isConnected() const { return !m_host.isEmpty() && m_session.holdsConnection(); }
    QString host() const { return m_host; }

    QString displayName() const override;
    QString scheme() const override { return QStringLiteral("smb"); }
    RemoteLocation remoteLocation() const override;
    QString shellAccessiblePath(const QString &path) const override;
    bool reconnect(QString *error) override;
    // Honoured by bounding the reachability probe that precedes the connect.
    // WNetAddConnection2 itself takes no timeout, and left to the OS an
    // unreachable host costs ~22s per attempt -- multiplied by the session's
    // retry budget before the user is told anything.
    void setTimeoutMs(int ms) override { m_timeoutMs = ms > 0 ? ms : m_timeoutMs; }

    bool reusedExistingSession() const override { return m_reusedSession; }
    QString reusedSessionUser() const override { return m_reusedSessionUser; }

    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    ListStatus lastListStatus() const override { return m_lastListStatus; }
    QString lastListError() const override { return m_lastListError; }
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;
    QString parentPath(const QString &path) const override;
    bool exists(const QString &path) const override;
    RenameResult rename(const QString &path, const QString &newName,
                        QString *newPath) override;
    RenameResult moveTo(const QString &srcPath, const QString &dstPath) override;

    FileHandle *openRead(const QString &path) override;
    FileHandle *openWrite(const QString &path, bool truncate) override;
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override;
    qint64 write(FileHandle *handle, const char *buffer, qint64 size) override;
    bool seek(FileHandle *handle, qint64 offset) override;
    qint64 handleSize(FileHandle *handle) override;
    void closeHandle(FileHandle *handle) override;
    bool canStream() const override { return true; }
    bool setModifiedTime(const QString &path, const QDateTime &modified) override;
    bool remove(const QString &path) override;
    bool mkdir(const QString &path) override;

    static QString normalizeProviderPath(const QString &path);
    static QString parentProviderPath(const QString &path);
    static QString providerPathToUnc(const QString &host, const QString &path,
                                     QString *error = nullptr);

private:
    QString uncFor(const QString &path, QString *error = nullptr) const;
    WindowsSmbSession::Result ensureShareFor(const QString &path,
                                              QString *error = nullptr) const;
    // ensureShareFor reduced to "may I touch this path". Callers that have no
    // way to report *why* (remove, mkdir, openRead, ...) use this; the ones that
    // do -- list() -- keep the full result so a denial can be named.
    bool shareReady(const QString &path) const {
        return ensureShareFor(path) == WindowsSmbSession::Result::Connected;
    }
    // Records how a list() ended so lastListStatus()/lastListError() can report
    // it. Called only from list(), on the session worker thread.
    void noteListResult(ListStatus status, const QString &error = QString()) const;

    QString m_host;
    QString m_user;
    QString m_password;
    QString m_workgroup;
    bool m_anonymous = false;
    int m_timeoutMs = 12000; // matches NetworkSession::kConnectTimeoutMs
    // Set when the connect adopted a session Windows already held, so the panel
    // can say whose identity the browse is actually running under.
    bool m_reusedSession = false;
    QString m_reusedSessionUser;
    mutable WindowsSmbSession m_session;
    // Written by list() and read immediately afterwards on the same worker
    // thread; see FileProvider::lastListStatus().
    mutable ListStatus m_lastListStatus = ListStatus::Ok;
    mutable QString m_lastListError;
};
