#include "TreeRootBuilder.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStorageInfo>

namespace {

// Filesystem types that are never a place the user browses files: kernel and
// container plumbing that QStorageInfo reports alongside real disks.
bool isPseudoFilesystem(const QByteArray &type) {
    static const QVector<QByteArray> kPseudo{"proc",     "sysfs",      "devtmpfs", "devpts",
                                             "tmpfs",    "cgroup",     "cgroup2",  "securityfs",
                                             "pstore",   "bpf",        "tracefs",  "debugfs",
                                             "configfs", "fusectl",    "mqueue",   "hugetlbfs",
                                             "squashfs", "ramfs",      "autofs",   "binfmt_misc",
                                             "efivarfs", "fuse.gvfsd-fuse"};
    return kPseudo.contains(type);
}

// The physical drive behind a partition device node: "/dev/nvme0n1p4" and
// "/dev/nvme0n1p5" are both "/dev/nvme0n1", "/dev/sda1" is "/dev/sda".
//
// This is the distinction the tree is actually about. A modern install spreads
// one disk across many mounts (/, /boot, /boot/efi, /home, /var, and on an
// ostree system another half-dozen), and every one of them is already reachable
// by walking down from "/". Listing them as sibling roots would both duplicate
// the tree and, far worse, mean a perfectly ordinary single-disk machine never
// takes the plain-filesystem path.
QString physicalDriveOf(const QByteArray &device) {
    const QString node = QString::fromUtf8(device);
    // NVMe names a partition controller+namespace+partition ("nvme0n1p4", whose
    // drive is "nvme0n1"), MMC just appends "p<N>" ("mmcblk0p1" -> "mmcblk0"),
    // and SCSI/SATA/IDE append the number directly ("sda1" -> "sda").
    static const QRegularExpression kPartitionForms[] = {
        QRegularExpression(QStringLiteral("^(/dev/nvme\\d+n\\d+)p\\d+$")),
        QRegularExpression(QStringLiteral("^(/dev/mmcblk\\d+)p\\d+$")),
        QRegularExpression(QStringLiteral("^(/dev/[a-zA-Z]+)\\d+$")),
    };
    for (const QRegularExpression &form : kPartitionForms) {
        const QRegularExpressionMatch match = form.match(node);
        if (match.hasMatch())
            return match.captured(1);
    }
    return node;
}

QString labelForVolume(const QStorageInfo &storage) {
    const QString name = storage.name();
    if (!name.isEmpty())
        return name;
    // An unlabelled volume: the mount point reads better than a bare device
    // node, except for the root filesystem where the device says more.
    const QString mount = storage.rootPath();
    if (mount != QDir::rootPath())
        return QFileInfo(mount).fileName().isEmpty() ? mount : QFileInfo(mount).fileName();
    return QString::fromUtf8(storage.device());
}

} // namespace

QVector<LocalVolume> TreeRootBuilder::enumerateLocalVolumes(const QStringList &removableMounts) {
    QVector<LocalVolume> volumes;
    QHash<QString, int> driveSlot; // physical drive -> index into `volumes`

    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady())
            continue;
        if (isPseudoFilesystem(storage.fileSystemType()))
            continue;
        // Only filesystems backed by a real block device are disks; loop mounts
        // (snap packages) and network mounts are not what "a hard disk" means
        // here, and network shares reach the tree through their provider.
        const QByteArray device = storage.device();
        if (!device.startsWith("/dev/") || device.startsWith("/dev/loop"))
            continue;
        if (removableMounts.contains(storage.rootPath()))
            continue;

        LocalVolume volume;
        volume.name = labelForVolume(storage);
        volume.mountPoint = storage.rootPath();

        // One root per physical drive, not per partition. Within a drive the
        // shallowest mount wins ("/" beats "/home", which beats "/home/x"):
        // it is the useful place to start, and the deeper mounts hang below it.
        const QString drive = physicalDriveOf(device);
        const auto slot = driveSlot.constFind(drive);
        if (slot == driveSlot.constEnd()) {
            driveSlot.insert(drive, volumes.size());
            volumes.append(volume);
        } else if (volume.mountPoint.length() < volumes.at(*slot).mountPoint.length()) {
            volumes[*slot] = volume;
        }
    }
    return volumes;
}

QStringList TreeRootBuilder::networkBaseCandidates(const QString &currentPath) {
    QStringList candidates{QStringLiteral("/")};
    QString accumulated;
    for (const QString &segment : currentPath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        accumulated += QLatin1Char('/') + segment;
        candidates.append(accumulated);
    }
    return candidates;
}

QVector<TreeRoot> TreeRootBuilder::build(const QVector<LocalVolume> &volumes,
                                         const QVector<RemovableDevice> &devices,
                                         const QVector<NetworkTreeEntry> &networks) {
    QVector<TreeRoot> roots;

    // Only mounted removable volumes can host a tree; an unmounted one has no
    // path to descend from. It still appears in the external-connection panel,
    // which can mount it on demand.
    QVector<RemovableDevice> mountedDevices;
    for (const RemovableDevice &device : devices)
        if (device.isMounted && !device.mountPoint.isEmpty())
            mountedDevices.append(device);

    // The common case: one disk, nothing plugged in, nothing connected. Keep the
    // plain whole-filesystem tree rather than wrapping it in a device row.
    if (volumes.size() <= 1 && mountedDevices.isEmpty() && networks.isEmpty()) {
        TreeRoot root;
        root.kind = TreeRoot::LocalFilesystem;
        root.label = volumes.isEmpty() ? QDir::rootPath() : volumes.first().name;
        root.iconName = QStringLiteral("dev-hdd");
        root.basePath = QDir::rootPath();
        roots.append(root);
        return roots;
    }

    for (const LocalVolume &volume : volumes) {
        TreeRoot root;
        root.kind = TreeRoot::LocalVolume;
        root.label = volume.name;
        root.iconName = QStringLiteral("dev-hdd");
        root.basePath = volume.mountPoint;
        roots.append(root);
    }

    for (const RemovableDevice &device : mountedDevices) {
        TreeRoot root;
        root.kind = TreeRoot::Removable;
        root.label = device.name;
        // The monitor already classifies the device (usb / hdd / sdcard / phone
        // / drive) and every alias has a matching :/icons/<name>.svg.
        root.iconName = device.iconName.isEmpty() ? QStringLiteral("dev-drive") : device.iconName;
        root.basePath = device.mountPoint;
        root.deviceId = device.id;
        roots.append(root);
    }

    for (const NetworkTreeEntry &network : networks) {
        TreeRoot root;
        root.kind = TreeRoot::Network;
        root.label = network.label;
        root.iconName = network.scheme.isEmpty() ? QStringLiteral("dev-drive")
                                                 : QStringLiteral("dev-") + network.scheme;
        root.basePath = network.basePath.isEmpty() ? QStringLiteral("/") : network.basePath;
        root.basePathFallbacks = network.basePathFallbacks;
        root.connectionId = network.connectionId;
        root.activatable = network.ownedByThisPanel;
        roots.append(root);
    }

    return roots;
}
