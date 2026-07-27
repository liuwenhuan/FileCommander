#pragma once

#include <QDateTime>
#include <QFile>
#include <QString>

class FileInfo {
public:
    FileInfo() = default;
    explicit FileInfo(const QString &path);

    // The ".." row for a listing whose parent is `parentPath`.
    //
    // `localFilesystem` is the backend's FileProvider::isLocalFilesystem(): only
    // then may the parent be stat'ed, because only then is `parentPath` a path
    // on this machine. On a network or archive backend it names a directory on
    // the server (or an entry inside an archive), and stat'ing it describes a
    // same-named LOCAL directory instead -- which is how ".." on a share in
    // "/home" came to show this machine's "/" timestamp, and how ".." with no
    // local namesake at all came out as "0 B", type "File", "----------".
    //
    // With `localFilesystem` false the row carries only what is actually known:
    // it is a directory, it is the parent, and nothing else. Size, timestamps
    // and permissions stay unset, so the columns render empty rather than wrong.
    static FileInfo makeParentEntry(const QString &parentPath, bool localFilesystem);

    // Builds a FileInfo from pre-fetched stat fields rather than probing the
    // local filesystem. Used by remote backends (e.g. SFTP) where a QFileInfo
    // over `path` would be meaningless. `created` is left invalid (SFTP has no
    // creation time) and isSymLink defaults to false.
    //
    // ownerId/groupId are numeric uid/gid (-1 when unknown); owner/group are
    // resolved names (often empty for remote backends that only expose numbers).
    // These trail the signature with defaults so existing call sites keep
    // compiling unchanged.
    static FileInfo fromFields(const QString &path, const QString &name, qint64 size,
                               const QDateTime &modified, bool isDir,
                               QFile::Permissions permissions, int ownerId = -1,
                               int groupId = -1, const QString &owner = QString(),
                               const QString &group = QString());

    // Splits a leaf name with POSIX hidden-file semantics: a leading dot alone
    // does not begin an extension, while `.config.json` has base `.config` and
    // suffix `json`.
    static QString baseNameForName(const QString &name);
    static QString suffixForName(const QString &name);

    const QString &name() const { return m_name; }        // full file name, e.g. "photo.jpg"
    const QString &baseName() const { return m_baseName; } // name without extension, e.g. "photo"
    const QString &path() const { return m_path; }
    const QString &suffix() const { return m_suffix; }
    qint64 size() const { return m_size; }
    const QDateTime &modified() const { return m_modified; }
    const QDateTime &created() const { return m_created; }
    QFile::Permissions permissions() const { return m_permissions; }
    // Numeric owner/group ids (-1 when unknown) and their resolved names (may be
    // empty when only numeric ids are available, as with SFTP).
    int ownerId() const { return m_ownerId; }
    int groupId() const { return m_groupId; }
    const QString &owner() const { return m_owner; }
    const QString &group() const { return m_group; }
    bool isDir() const { return m_isDir; }
    bool isSymLink() const { return m_isSymLink; }
    bool isParentEntry() const { return m_isParentEntry; }
    // Computed lazily on first access (MIME detection is relatively expensive
    // and most listings never need it), then cached.
    const QString &mimeType() const;

    // Whether permissions() means anything. False only for a ".." row on a
    // backend that cannot be stat'ed (see makeParentEntry): there, a bit-pattern
    // of zero would render as "----------", i.e. "nobody may read this", which
    // is a claim about the server nothing here is in a position to make.
    bool hasPermissions() const { return m_permissionsKnown; }
    // The "drwxr-xr-x" form, or an empty string when permissions are unknown.
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
    QFile::Permissions m_permissions = {};
    bool m_permissionsKnown = true;
    int m_ownerId = -1;
    int m_groupId = -1;
    QString m_owner;
    QString m_group;
    bool m_isDir = false;
    bool m_isSymLink = false;
    bool m_isParentEntry = false;
    mutable QString m_mimeType; // lazily populated by mimeType()
};

// So QVector<FileInfo> can cross a queued signal/slot connection (the network
// session delivers directory listings from its worker thread to the GUI thread).
Q_DECLARE_METATYPE(FileInfo)
