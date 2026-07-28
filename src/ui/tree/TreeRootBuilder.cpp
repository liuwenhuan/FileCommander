#include "TreeRootBuilder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace {

// mountinfo escapes whitespace and backslashes as three-digit octal sequences.
// Decode the field without resolving or statting the path: that is the property
// this parser exists to preserve when a network mount has stopped responding.
QByteArray decodeMountField(const QByteArray &field) {
    QByteArray decoded;
    decoded.reserve(field.size());
    for (int i = 0; i < field.size(); ++i) {
        if (field.at(i) == '\\' && i + 3 < field.size()
            && field.at(i + 1) >= '0' && field.at(i + 1) <= '7'
            && field.at(i + 2) >= '0' && field.at(i + 2) <= '7'
            && field.at(i + 3) >= '0' && field.at(i + 3) <= '7') {
            const int value = (field.at(i + 1) - '0') * 64
                              + (field.at(i + 2) - '0') * 8
                              + (field.at(i + 3) - '0');
            decoded.append(static_cast<char>(value));
            i += 3;
        } else {
            decoded.append(field.at(i));
        }
    }
    return decoded;
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

QString labelForVolume(const QByteArray &device, const QString &mount) {
    // Keep this purely lexical. QFileInfo::fileName() does not query the path,
    // whereas QStorageInfo::name()/isReady() may call statfs and hang on a dead
    // mount before the main window has appeared.
    if (mount != QDir::rootPath()) {
        const QString name = QFileInfo(mount).fileName();
        if (!name.isEmpty())
            return name;
    }
    return QString::fromUtf8(device);
}

} // namespace

QVector<LocalVolume> TreeRootBuilder::localVolumesFromMountInfo(
    const QByteArray &mountInfo, const QStringList &removableMounts) {
    QVector<LocalVolume> volumes;
    QHash<QString, int> driveSlot; // physical drive -> index into `volumes`

    for (const QByteArray &line : mountInfo.split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        // Fields 0..5 are fixed, then zero or more optional fields, then a lone
        // "-" separator followed by filesystem type, mount source and options.
        const int separator = fields.indexOf(QByteArrayLiteral("-"));
        if (fields.size() < 7 || separator < 6 || separator + 2 >= fields.size())
            continue;

        const QByteArray fileSystemType = fields.at(separator + 1);
        if (fileSystemType == QByteArrayLiteral("fuse")
            || fileSystemType.startsWith(QByteArrayLiteral("fuse.")))
            continue;

        // A FUSE mount controls its displayed source name and can call itself
        // "/dev/sdc1". The kernel-assigned major:minor field is not forgeable in
        // that way: anonymous/network filesystems use major 0, block devices do
        // not. Require both signals before treating a record as a local disk.
        const int colon = fields.at(2).indexOf(':');
        bool majorOk = false;
        const uint major = colon > 0 ? fields.at(2).left(colon).toUInt(&majorOk) : 0;
        if (!majorOk || major == 0)
            continue;

        const QByteArray device = decodeMountField(fields.at(separator + 2));
        // Filter by source before doing anything with the mount point. CIFS,
        // NFS, GVFS/FUSE, overlay and virtual filesystems do not use /dev nodes,
        // so a dead network mount is discarded using mount-table text alone.
        if (!device.startsWith("/dev/") || device.startsWith("/dev/loop"))
            continue;

        const QString mountPoint = QString::fromUtf8(decodeMountField(fields.at(4)));
        if (mountPoint.isEmpty() || removableMounts.contains(mountPoint))
            continue;

        LocalVolume volume;
        volume.name = labelForVolume(device, mountPoint);
        volume.mountPoint = mountPoint;

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

QVector<LocalVolume> TreeRootBuilder::enumerateLocalVolumes(const QStringList &removableMounts) {
    QFile mountInfo(QStringLiteral("/proc/self/mountinfo"));
    if (!mountInfo.open(QIODevice::ReadOnly))
        return {};
    return localVolumesFromMountInfo(mountInfo.readAll(), removableMounts);
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
