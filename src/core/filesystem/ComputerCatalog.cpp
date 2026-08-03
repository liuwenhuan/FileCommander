#include "ComputerCatalog.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>

#include "network/ConnectionStore.h"

namespace {

// Filesystems that QStorageInfo reports on Linux but that are not drives in the
// sense the user means: kernel bookkeeping, RAM-backed scratch space, and the
// squashfs loopbacks snap packages mount (a stock Ubuntu desktop has dozens).
// Windows never produces these, so the set is simply empty there in practice.
bool isPseudoFilesystem(const QByteArray &type) {
    static const QSet<QByteArray> pseudo = {
        "tmpfs",  "devtmpfs", "squashfs", "overlay",  "proc",     "sysfs",
        "cgroup", "cgroup2",  "devpts",   "ramfs",    "autofs",   "efivarfs",
        "fuse.gvfsd-fuse",    "fuse.portal",          "tracefs",  "debugfs",
        "securityfs",         "pstore",   "bpf",      "configfs", "hugetlbfs",
        "mqueue",             "fusectl",  "nsfs",     "binfmt_misc",
    };
    return pseudo.contains(type);
}

QString driveIconPath() { return QStringLiteral(":/icons/dev-drive.svg"); }

// Mirrors ExternalConnectDialog's mapping, kept here so the computer listing and
// the connect fly-out cannot drift apart on what an SFTP row looks like.
QString iconForProtocol(int protocol) {
    switch (protocol) {
    case 0: // Sftp
        return QStringLiteral(":/icons/dev-sftp.svg");
    case 1: // Smb
        return QStringLiteral(":/icons/dev-smb.svg");
    case 2: // WebDav
    case 3: // WebDavs
        return QStringLiteral(":/icons/dev-webdav.svg");
    case 4: // Ftp
        return QStringLiteral(":/icons/dev-ftp.svg");
    default:
        return QStringLiteral(":/icons/dev-smb.svg");
    }
}

} // namespace

QVector<ComputerEntry> ComputerCatalog::drives() {
    QVector<ComputerEntry> result;
    QSet<QString> seenRoots;
    QSet<QString> seenVolumes;

    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        if (isPseudoFilesystem(volume.fileSystemType()))
            continue;

        const QString root = QDir::fromNativeSeparators(volume.rootPath());
        if (root.isEmpty() || seenRoots.contains(root))
            continue;

        const QString device = QString::fromLocal8Bit(volume.device());
        // One disk, listed once. A bind mount shows the same device at a second
        // mount point (WSL does this for /mnt/wslg/distro, and any /etc/fstab
        // "bind" entry does it too), and listing both reads as two disks that
        // are really one. The subvolume is part of the identity so that btrfs
        // subvolumes -- genuinely separate places on one device -- stay
        // distinct, and the FIRST mount wins because that is the shortest, most
        // canonical path to the volume.
        const QString volumeId =
            device + QLatin1Char('\0') + QString::fromLocal8Bit(volume.subvolume());
        if (!device.isEmpty() && seenVolumes.contains(volumeId))
            continue;
#ifndef Q_OS_WIN
        // Everything real on Linux is backed by a node under /dev. This is the
        // one test that removes the long tail of pseudo mounts the blacklist
        // above does not enumerate, without having to keep that list exhaustive.
        if (!device.startsWith(QLatin1String("/dev/")))
            continue;
#endif

        ComputerEntry entry;
        entry.kind = ComputerEntry::Kind::Drive;
        entry.target = root;
        entry.iconPath = driveIconPath();
        entry.bytesTotal = volume.bytesTotal();
        entry.bytesFree = volume.bytesFree();

        const QString label = volume.name();
#ifdef Q_OS_WIN
        // "C:" -- the letter is the identity here, and the label qualifies it,
        // which is the order Windows itself shows ("Windows (C:)").
        QString letter = root;
        while (letter.endsWith(QLatin1Char('/')))
            letter.chop(1);
        entry.name = QStringLiteral("%1 (%2)")
                         .arg(label.isEmpty() ? QObject::tr("Local Disk") : label, letter);
#else
        // On Linux the device node is the drive's identity; the label (when the
        // filesystem carries one) and the mount point qualify it. An unlabelled
        // volume falls back to where it is mounted, which is the only other
        // thing that distinguishes two anonymous partitions.
        const QString identity = label.isEmpty() ? root : label;
        entry.name = QStringLiteral("%1 (%2)").arg(identity, device);
#endif
        seenRoots.insert(root);
        seenVolumes.insert(volumeId);
        result.append(entry);
    }

    // QStorageInfo enumerates in whatever order the OS reports mounts, which is
    // neither stable nor meaningful -- Windows commonly returns D: before C:.
    // Sort by the mount root, which is the traditional order on both platforms:
    // "C:/" < "D:/" < "E:/", and on Linux "/" sorts before "/home" and
    // "/mnt/..." because it is their prefix. Deliberately NOT by the display
    // name: that would order Windows disks by volume label, so a disk called
    // "ntfs" would come before the system disk.
    std::sort(result.begin(), result.end(),
              [](const ComputerEntry &a, const ComputerEntry &b) {
                  return QString::compare(a.target, b.target, Qt::CaseInsensitive) < 0;
              });
    return result;
}

QVector<ComputerEntry> ComputerCatalog::userFolders() {
    struct Standard {
        QStandardPaths::StandardLocation location;
        QString name;
    };
    // Named as the user thinks of them ("My Documents"), and in the order the
    // shell conventionally lists them rather than alphabetically.
    const QVector<Standard> standards = {
        {QStandardPaths::DesktopLocation, QObject::tr("My Desktop")},
        {QStandardPaths::DocumentsLocation, QObject::tr("My Documents")},
        {QStandardPaths::PicturesLocation, QObject::tr("My Pictures")},
        {QStandardPaths::MusicLocation, QObject::tr("My Music")},
        {QStandardPaths::MoviesLocation, QObject::tr("My Videos")},
        {QStandardPaths::DownloadLocation, QObject::tr("My Downloads")},
    };

    QVector<ComputerEntry> result;
    QSet<QString> seen;
    for (const Standard &standard : standards) {
        const QString path = QStandardPaths::writableLocation(standard.location);
        // Skipped rather than shown greyed out: a location the platform does not
        // define comes back empty, and one the user deleted would navigate to
        // nothing. Both are better left out than offered and broken.
        if (path.isEmpty() || !QFileInfo(path).isDir())
            continue;
        const QString cleaned = QDir::cleanPath(path);
        if (seen.contains(cleaned))
            continue; // some setups point two standard locations at $HOME
        seen.insert(cleaned);

        ComputerEntry entry;
        entry.kind = ComputerEntry::Kind::UserFolder;
        entry.name = standard.name;
        entry.target = cleaned;
        // Left empty on purpose: IconCache gives it the same folder icon the
        // rest of the application uses, so it reads as a folder and follows the
        // icon theme.
        result.append(entry);
    }
    return result;
}

QVector<ComputerEntry> ComputerCatalog::savedServers() {
    QVector<ComputerEntry> result;
    const QVector<SavedConnection> saved = ConnectionStore::loadAll();
    result.reserve(saved.size());
    for (const SavedConnection &connection : saved) {
        ComputerEntry entry;
        entry.kind = ComputerEntry::Kind::SavedServer;
        entry.name = connection.name.isEmpty() ? connection.host : connection.name;
        entry.target = connection.id;
        entry.iconPath = iconForProtocol(connection.protocol);
        entry.created = connection.created;
        result.append(entry);
    }
    return result;
}

QVector<ComputerEntry> ComputerCatalog::localAndSaved() {
    QVector<ComputerEntry> result = drives();
    result += userFolders();
    result += savedServers();
    return result;
}
