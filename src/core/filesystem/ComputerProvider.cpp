#include "ComputerProvider.h"

#include <QMutexLocker>
#include <QObject>

namespace {

QString kindTag(ComputerEntry::Kind kind) {
    switch (kind) {
    case ComputerEntry::Kind::Drive:
        return QStringLiteral("drive");
    case ComputerEntry::Kind::UserFolder:
        return QStringLiteral("folder");
    case ComputerEntry::Kind::RemovableDevice:
        return QStringLiteral("device");
    case ComputerEntry::Kind::SavedServer:
        return QStringLiteral("server");
    case ComputerEntry::Kind::NetworkHost:
        return QStringLiteral("host");
    }
    return QStringLiteral("entry");
}

// Section order. Local storage first (what the user reaches for most), then the
// personal folders, then the things that need a network or a plug: removable
// media, saved bookmarks, and finally hosts that merely happen to be reachable.
int sortGroup(ComputerEntry::Kind kind) {
    switch (kind) {
    case ComputerEntry::Kind::Drive:
        return 0;
    case ComputerEntry::Kind::UserFolder:
        return 1;
    case ComputerEntry::Kind::RemovableDevice:
        return 2;
    case ComputerEntry::Kind::SavedServer:
        return 3;
    case ComputerEntry::Kind::NetworkHost:
        return 4;
    }
    return 5;
}

QString typeLabel(ComputerEntry::Kind kind) {
    switch (kind) {
    case ComputerEntry::Kind::Drive:
        return QObject::tr("Drive");
    case ComputerEntry::Kind::UserFolder:
        return QObject::tr("Folder");
    case ComputerEntry::Kind::RemovableDevice:
        return QObject::tr("Removable Device");
    case ComputerEntry::Kind::SavedServer:
    case ComputerEntry::Kind::NetworkHost:
        return QObject::tr("Server");
    }
    return QObject::tr("Folder");
}

} // namespace

QString ComputerProvider::rootPath() { return QStringLiteral("computer://"); }

bool ComputerProvider::isComputerPath(const QString &path) {
    return path.startsWith(rootPath());
}

QString ComputerProvider::pathForEntry(const ComputerEntry &entry) {
    return rootPath() + kindTag(entry.kind) + QLatin1Char('/') + entry.target;
}

void ComputerProvider::setEntries(const QVector<ComputerEntry> &entries) {
    QMutexLocker locker(&m_mutex);
    m_entries = entries;
    m_byPath.clear();
    m_byPath.reserve(entries.size());
    for (const ComputerEntry &entry : entries)
        m_byPath.insert(pathForEntry(entry), entry);
}

ComputerEntry ComputerProvider::entryFor(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    return m_byPath.value(path);
}

QVector<FileInfo> ComputerProvider::list(const QString &path, bool /*showHidden*/) const {
    if (cleanPath(path) != rootPath())
        return {}; // nothing below the root; every row navigates elsewhere

    QMutexLocker locker(&m_mutex);
    QVector<FileInfo> result;
    result.reserve(m_entries.size());
    for (const ComputerEntry &entry : m_entries) {
        // isDir: every row opens into something, so the views treat them as
        // navigable and never offer to preview or edit one. Permissions are
        // reported as unknown-but-readable rather than left at zero, which would
        // render as "----------", i.e. a claim that the user cannot read their
        // own desktop.
        result.append(FileInfo::fromFields(
            pathForEntry(entry), entry.name, /*size=*/0, /*modified=*/QDateTime(),
            /*isDir=*/true,
            QFile::ReadOwner | QFile::ExeOwner | QFile::ReadUser | QFile::ExeUser,
            /*ownerId=*/-1, /*groupId=*/-1, /*owner=*/QString(), /*group=*/QString(),
            entry.created));
    }
    return result;
}

bool ComputerProvider::isDir(const QString &path) const {
    const QString cleaned = cleanPath(path);
    if (cleaned == rootPath())
        return true;
    QMutexLocker locker(&m_mutex);
    return m_byPath.contains(cleaned);
}

QString ComputerProvider::cleanPath(const QString &path) const {
    // No normalisation: these are opaque identifiers, not paths, and running
    // QDir::cleanPath over one would collapse the "//" in the scheme.
    return path;
}

QString ComputerProvider::parentPath(const QString &path) const {
    // The root has no parent, so the listing gets no ".." row. A row's parent is
    // the root, but rows are never listed into, so this only matters for the
    // model's own bookkeeping.
    return cleanPath(path) == rootPath() ? QString() : rootPath();
}

bool ComputerProvider::exists(const QString &path) const { return isDir(path); }

FileProvider::RenameResult ComputerProvider::rename(const QString & /*path*/,
                                                    const QString & /*newName*/,
                                                    QString * /*newPath*/) {
    // Failed, not Unsupported: Unsupported means "ask another backend", and
    // there is no other backend that could rename a drive.
    return RenameResult::Failed;
}

QString ComputerProvider::entryTypeLabel(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    return it == m_byPath.constEnd() ? QString() : typeLabel(it->kind);
}

QString ComputerProvider::entryIconPath(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    return m_byPath.value(path).iconPath;
}

int ComputerProvider::entrySortGroup(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    return it == m_byPath.constEnd() ? 0 : sortGroup(it->kind);
}
