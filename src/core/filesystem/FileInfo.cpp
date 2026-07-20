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
    m_suffix = m_isDir ? QString() : qfi.suffix();
    m_size = qfi.size();
    m_modified = qfi.lastModified();
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

FileInfo FileInfo::makeParentEntry(const QString &parentPath) {
    FileInfo info(parentPath);
    info.m_name = QStringLiteral("..");
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
