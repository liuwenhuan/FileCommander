#include "ComputerCatalog.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QHash>
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

#ifndef Q_OS_WIN
// Mount points that belong to the operating system rather than to the user.
// These are real block-device filesystems, so isPseudoFilesystem() cannot
// remove them, but nobody browses to /boot or /var from a file manager and an
// ostree layout (Deepin's) mounts a handful of them off one partition, which is
// what turned two disks into nine rows.
bool isSystemMountPoint(const QString &root) {
    // The root filesystem is never hidden: it is the one mount every machine
    // has and the only way to reach anything not under a more specific mount.
    if (root == QLatin1String("/"))
        return false;

    static const QStringList system = {
        QStringLiteral("/boot"),   QStringLiteral("/efi"),      QStringLiteral("/sysroot"),
        QStringLiteral("/ostree"), QStringLiteral("/persistent"), QStringLiteral("/var"),
        QStringLiteral("/usr"),    QStringLiteral("/root"),     QStringLiteral("/recovery"),
        QStringLiteral("/run"),    QStringLiteral("/snap"),     QStringLiteral("/proc"),
        QStringLiteral("/sys"),    QStringLiteral("/dev"),      QStringLiteral("/tmp"),
        QStringLiteral("/etc"),
    };
    for (const QString &prefix : system) {
        // Equal, or below it: /persistent/ostree is covered by /persistent, but
        // a user directory called /vares must not be covered by /var.
        if (root == prefix || root.startsWith(prefix + QLatin1Char('/')))
            return true;
    }
    return false;
}

// Only these can put two genuinely separate places on one device. Everywhere
// else a non-empty subvolume -- which is what QStorageInfo reports for the
// mountinfo root field -- means a bind mount, i.e. the same place seen twice.
bool hasRealSubvolumes(const QByteArray &type) {
    return type == "btrfs" || type == "bcachefs";
}
#endif

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

QVector<ComputerEntry> ComputerCatalog::drives(bool includeSystemVolumes) {
    QVector<ComputerEntry> result;
    QSet<QString> seenRoots;
    // Device node -> index into result, so a later mount of a device already
    // listed can be compared against the one that is in and replace it.
    QHash<QString, int> deviceRow;
    QHash<QString, QString> chosenSubvolume;

    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        if (isPseudoFilesystem(volume.fileSystemType()))
            continue;

        const QString root = QDir::fromNativeSeparators(volume.rootPath());
        if (root.isEmpty() || seenRoots.contains(root))
            continue;

        const QString device = QString::fromLocal8Bit(volume.device());
        const QString subvolume = QString::fromLocal8Bit(volume.subvolume());
#ifndef Q_OS_WIN
        // Everything real on Linux is backed by a node under /dev. This is the
        // one test that removes the long tail of pseudo mounts the blacklist
        // above does not enumerate, without having to keep that list exhaustive.
        if (!device.startsWith(QLatin1String("/dev/")))
            continue;
        if (!includeSystemVolumes && isSystemMountPoint(root))
            continue;
        // One disk, listed once. A bind mount shows the same device at a second
        // mount point (an ostree root does it for /home, /var and /root off one
        // partition; WSL does it for /mnt/wslg/distro; any /etc/fstab "bind"
        // entry does it too), and listing both reads as several disks that are
        // really one. The subvolume joins the identity only on a filesystem
        // that can actually carry subvolumes: elsewhere QStorageInfo reports
        // the mountinfo root field there, which differs per bind mount and
        // would defeat the deduplication it is meant to refine.
        const QString volumeId = hasRealSubvolumes(volume.fileSystemType())
                                     ? device + QLatin1Char('\0') + subvolume
                                     : device;
#else
        const QString volumeId = device + QLatin1Char('\0') + subvolume;
#endif

        ComputerEntry entry;
        entry.kind = ComputerEntry::Kind::Drive;
        entry.target = root;
        entry.iconPath = driveIconPath();
        entry.bytesTotal = volume.bytesTotal();
        entry.bytesFree = volume.bytesFree();

#ifdef Q_OS_WIN
        const QString label = volume.name();
        // "C:" -- the letter is the identity here, and the label qualifies it,
        // which is the order Windows itself shows ("Windows (C:)").
        QString letter = root;
        while (letter.endsWith(QLatin1Char('/')))
            letter.chop(1);
        entry.name = QStringLiteral("%1 (%2)")
                         .arg(label.isEmpty() ? QObject::tr("Local Disk") : label, letter);
#else
        // On Linux the mount point is what the user navigates to, and it is the
        // whole name: the device node this used to show ("/dev/nvme0n1p5") and
        // the filesystem label an installer wrote ("_dde_data", "Roota") are
        // both identifiers for the same volume that mean nothing to whoever is
        // reading the list. A removable disk keeps its label anyway, because the
        // mount point contains it (/media/deepin/KINGSTON).
        entry.name = root;
#endif
        seenRoots.insert(root);

        // Same volume as a row already produced: keep whichever mount point is
        // the better name for it -- the whole-filesystem mount over a bind mount
        // of a subtree, then the shortest path, which is the most canonical way
        // in. (In includeSystemVolumes mode there is no deduplication at all:
        // showing every mount of a disk is the entire point of that mode.)
        if (!includeSystemVolumes && !device.isEmpty()) {
            const auto existing = deviceRow.constFind(volumeId);
            if (existing != deviceRow.constEnd()) {
                const int row = existing.value();
                const QString incumbentSub = chosenSubvolume.value(volumeId);
                const bool better = (incumbentSub.isEmpty() != subvolume.isEmpty())
                                        ? subvolume.isEmpty()
                                        : root.length() < result.at(row).target.length();
                if (better) {
                    result[row] = entry;
                    chosenSubvolume[volumeId] = subvolume;
                }
                continue;
            }
            deviceRow.insert(volumeId, result.size());
            chosenSubvolume.insert(volumeId, subvolume);
        }
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

    // Last, and only once something has arrived (or sharing created it): an
    // empty row for a directory that does not exist would navigate to nothing.
    const QString received = receivedFilesPath();
    if (QFileInfo(received).isDir()) {
        ComputerEntry entry;
        entry.kind = ComputerEntry::Kind::UserFolder;
        entry.name = QObject::tr("Files I Received");
        entry.target = received;
        result.append(entry);
    }
    return result;
}

QString ComputerCatalog::receivedFilesPath() {
    return QDir::homePath() + QLatin1String("/ReceivedFiles");
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

QVector<ComputerEntry> ComputerCatalog::localAndSaved(bool includeSystemVolumes) {
    QVector<ComputerEntry> result = drives(includeSystemVolumes);
    result += userFolders();
    result += savedServers();
    return result;
}
