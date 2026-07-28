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
    bool isConnected() const { return !m_host.isEmpty(); }
    QString host() const { return m_host; }

    QString displayName() const override;
    QString scheme() const override { return QStringLiteral("smb"); }
    RemoteLocation remoteLocation() const override;
    bool reconnect(QString *error) override;

    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
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
    bool ensureShareFor(const QString &path, QString *error = nullptr) const;

    QString m_host;
    QString m_user;
    QString m_password;
    QString m_workgroup;
    bool m_anonymous = false;
    mutable WindowsSmbSession m_session;
};
