#include "FileInfo.h"

#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>

FileInfo::FileInfo(const QString &path) {
    QFileInfo qfi(path);
    m_name = qfi.fileName();
    m_path = qfi.absoluteFilePath();
    m_isDir = qfi.isDir();
    m_isSymLink = qfi.isSymLink();
    // Directories are treated as having no extension: the whole name is the
    // base. For files, split off the trailing suffix so the Name and Ext
    // columns show complementary halves (e.g. "photo" + "jpg").
    m_suffix = m_isDir ? QString() : qfi.suffix();
    m_baseName = m_isDir ? m_name : qfi.completeBaseName();
    m_size = qfi.size();
    m_modified = qfi.lastModified();
    m_created = qfi.birthTime();
    if (!m_created.isValid())
        m_created = qfi.metadataChangeTime(); // birth time isn't always available
    m_permissions = qfi.permissions();
    // m_mimeType is intentionally left empty here; see mimeType() below.
}

const QString &FileInfo::mimeType() const {
    // Scanning a large directory constructs thousands of FileInfos; running
    // MIME detection on each one there dominated the load time even though
    // most listings never read it. Compute on demand instead, then cache.
    if (m_mimeType.isNull() && !m_isDir && !m_path.isEmpty()) {
        static QMimeDatabase db;
        m_mimeType = db.mimeTypeForFile(m_path).name();
    }
    return m_mimeType;
}

FileInfo FileInfo::fromFields(const QString &path, const QString &name, qint64 size,
                              const QDateTime &modified, bool isDir,
                              QFile::Permissions permissions) {
    FileInfo info;
    info.m_name = name;
    info.m_path = path;
    info.m_isDir = isDir;
    info.m_isSymLink = false;
    // Match the local constructor's split rule: directories have no extension
    // (the whole name is the base); files show base + trailing suffix.
    if (isDir) {
        info.m_suffix = QString();
        info.m_baseName = name;
    } else {
        const QFileInfo qfi(name);
        info.m_suffix = qfi.suffix();
        info.m_baseName = qfi.completeBaseName();
    }
    info.m_size = size;
    info.m_modified = modified;
    // m_created intentionally left invalid: SFTP exposes no creation time.
    info.m_permissions = permissions;
    return info;
}

FileInfo FileInfo::makeParentEntry(const QString &parentPath) {
    FileInfo info(parentPath);
    info.m_name = QStringLiteral("..");
    info.m_baseName = QStringLiteral("..");
    info.m_isParentEntry = true;
    return info;
}

QString FileInfo::permissionsString() const {
    QString s;
    s += m_isDir ? QLatin1Char('d') : (m_isSymLink ? QLatin1Char('l') : QLatin1Char('-'));
    s += (m_permissions & QFile::ReadOwner) ? QLatin1Char('r') : QLatin1Char('-');
    s += (m_permissions & QFile::WriteOwner) ? QLatin1Char('w') : QLatin1Char('-');
    s += (m_permissions & QFile::ExeOwner) ? QLatin1Char('x') : QLatin1Char('-');
    s += (m_permissions & QFile::ReadGroup) ? QLatin1Char('r') : QLatin1Char('-');
    s += (m_permissions & QFile::WriteGroup) ? QLatin1Char('w') : QLatin1Char('-');
    s += (m_permissions & QFile::ExeGroup) ? QLatin1Char('x') : QLatin1Char('-');
    s += (m_permissions & QFile::ReadOther) ? QLatin1Char('r') : QLatin1Char('-');
    s += (m_permissions & QFile::WriteOther) ? QLatin1Char('w') : QLatin1Char('-');
    s += (m_permissions & QFile::ExeOther) ? QLatin1Char('x') : QLatin1Char('-');
    return s;
}
