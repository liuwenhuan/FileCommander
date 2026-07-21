#pragma once

#include <QDateTime>
#include <QFile>
#include <QString>

class FileInfo {
public:
    FileInfo() = default;
    explicit FileInfo(const QString &path);

    static FileInfo makeParentEntry(const QString &parentPath);

    // Builds a FileInfo from pre-fetched stat fields rather than probing the
    // local filesystem. Used by remote backends (e.g. SFTP) where a QFileInfo
    // over `path` would be meaningless. `created` is left invalid (SFTP has no
    // creation time) and isSymLink defaults to false.
    static FileInfo fromFields(const QString &path, const QString &name, qint64 size,
                               const QDateTime &modified, bool isDir,
                               QFile::Permissions permissions);

    const QString &name() const { return m_name; }        // full file name, e.g. "photo.jpg"
    const QString &baseName() const { return m_baseName; } // name without extension, e.g. "photo"
    const QString &path() const { return m_path; }
    const QString &suffix() const { return m_suffix; }
    qint64 size() const { return m_size; }
    const QDateTime &modified() const { return m_modified; }
    const QDateTime &created() const { return m_created; }
    QFile::Permissions permissions() const { return m_permissions; }
    bool isDir() const { return m_isDir; }
    bool isSymLink() const { return m_isSymLink; }
    bool isParentEntry() const { return m_isParentEntry; }
    // Computed lazily on first access (MIME detection is relatively expensive
    // and most listings never need it), then cached.
    const QString &mimeType() const;

    QString permissionsString() const;
    bool isValid() const { return !m_path.isEmpty(); }

private:
    QString m_name;
    QString m_baseName;
    QString m_path;
    QString m_suffix;
    qint64 m_size = 0;
    QDateTime m_modified;
    QDateTime m_created;
    QFile::Permissions m_permissions;
    bool m_isDir = false;
    bool m_isSymLink = false;
    bool m_isParentEntry = false;
    mutable QString m_mimeType; // lazily populated by mimeType()
};
