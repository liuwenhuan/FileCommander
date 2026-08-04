#include "ComputerProvider.h"

#include <QDir>
#include <QMutexLocker>
#include <QObject>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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

// Same 1024-based, one-decimal form the file listing uses, so a drive's numbers
// read the same way as everything else in the column.
QString humanBytes(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 5) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(size, 0, 'f', 1).arg(QLatin1String(units[unit]));
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

bool ComputerProvider::entryIsRenameable(const QString &path) const {
#ifdef Q_OS_WIN
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    // Only a drive, and only its label. Everything else in this listing is a
    // name we invented (a bookmark, a discovered host) or one the system owns
    // and offers no way to set.
    return it != m_byPath.constEnd() && it->kind == ComputerEntry::Kind::Drive;
#else
    // Relabelling ext4/btrfs/xfs means e2label/btrfs/xfs_admin as root -- not
    // something a file manager should be spawning under sudo. Offering an
    // editor that always failed would be worse than offering none.
    Q_UNUSED(path);
    return false;
#endif
}

QString ComputerProvider::entryRenameSeed(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    // The label alone, never the display name: "Windows (C:)" would put the
    // drive letter in the editor, and the letter is not the user's to change.
    return it == m_byPath.constEnd() ? QString() : it->label;
}

FileProvider::RenameResult ComputerProvider::rename(const QString &path, const QString &newName,
                                                    QString *newPath) {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.find(path);
    // Failed, not Unsupported: Unsupported means "ask another backend", and
    // there is no other backend that could rename a row in this listing.
    if (it == m_byPath.end() || it->kind != ComputerEntry::Kind::Drive)
        return RenameResult::Failed;

#ifdef Q_OS_WIN
    // The volume is addressed by its own root, which comes from the catalog and
    // never from anything the user typed -- so a rename here cannot reach the
    // drive letter no matter what was entered. Only the label changes.
    const QString root = QDir::toNativeSeparators(
        it->target.endsWith(QLatin1Char('/')) ? it->target : it->target + QLatin1Char('/'));
    if (!SetVolumeLabelW(reinterpret_cast<const wchar_t *>(root.utf16()),
                         newName.isEmpty() ? nullptr
                                           : reinterpret_cast<const wchar_t *>(newName.utf16())))
        return RenameResult::Failed; // no write access to the volume root, or a rejected label

    it->label = newName;
    it->name = ComputerCatalog::driveDisplayName(it->target, QString(), it->label);
    for (ComputerEntry &entry : m_entries) {
        if (entry.kind == ComputerEntry::Kind::Drive && entry.target == it->target) {
            entry.label = it->label;
            entry.name = it->name;
        }
    }
    // The row keeps its path: a drive is identified by where it is mounted, and
    // relabelling does not move it.
    if (newPath)
        *newPath = path;
    return RenameResult::Ok;
#else
    Q_UNUSED(newName);
    Q_UNUSED(newPath);
    return RenameResult::Failed;
#endif
}

QString ComputerProvider::entryTypeLabel(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    return it == m_byPath.constEnd() ? QString() : typeLabel(it->kind);
}

QString ComputerProvider::entrySizeText(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    if (it == m_byPath.constEnd() || it->bytesTotal <= 0)
        return {}; // not a drive, or a volume that would not report its size
    // Used space only, to match what the column holds for every other row: one
    // size. The capacity it is measured against goes to the status line, where
    // the panel already reports the disk figures for wherever it is pointed.
    // bytesFree can exceed total on an unreadable volume, so the subtraction is
    // clamped rather than allowed to go negative.
    const qint64 used = qMax<qint64>(0, it->bytesTotal - qMax<qint64>(0, it->bytesFree));
    return humanBytes(used);
}

QString ComputerProvider::entryIconPath(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    return m_byPath.value(path).iconPath;
}

QString ComputerProvider::entrySystemIconPath(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    // Only a drive: its target is a mount root the platform knows about. Every
    // other kind's target is a saved-connection id, a bookmark, or a host name
    // -- nothing to ask the system about, and asking anyway would be the
    // "path == local filesystem path" mistake.
    if (it == m_byPath.constEnd() || it->kind != ComputerEntry::Kind::Drive)
        return {};
    return it->target;
}

int ComputerProvider::entrySortGroup(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    auto it = m_byPath.constFind(path);
    return it == m_byPath.constEnd() ? 0 : sortGroup(it->kind);
}
