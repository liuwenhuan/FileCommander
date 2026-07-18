#pragma once

#include <QDateTime>
#include <QFile>
#include <QString>

class FileInfo {
public:
    FileInfo() = default;
    explicit FileInfo(const QString &path);

    static FileInfo makeParentEntry(const QString &parentPath);

    const QString &name() const { return m_name; }
    const QString &path() const { return m_path; }
    const QString &suffix() const { return m_suffix; }
    qint64 size() const { return m_size; }
    const QDateTime &modified() const { return m_modified; }
    QFile::Permissions permissions() const { return m_permissions; }
    bool isDir() const { return m_isDir; }
    bool isSymLink() const { return m_isSymLink; }
    bool isParentEntry() const { return m_isParentEntry; }
    const QString &mimeType() const { return m_mimeType; }

    QString permissionsString() const;
    bool isValid() const { return !m_path.isEmpty(); }

private:
    QString m_name;
    QString m_path;
    QString m_suffix;
    qint64 m_size = 0;
    QDateTime m_modified;
    QFile::Permissions m_permissions;
    bool m_isDir = false;
    bool m_isSymLink = false;
    bool m_isParentEntry = false;
    QString m_mimeType;
};
