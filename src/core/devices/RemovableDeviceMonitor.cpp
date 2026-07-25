#include "RemovableDeviceMonitor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QHash>
#include <QTimer>
#include <QVariantMap>

namespace {

// UDisks2 D-Bus names.
const char *kService = "org.freedesktop.UDisks2";
const char *kManagerPath = "/org/freedesktop/UDisks2";
const char *kObjectManagerIface = "org.freedesktop.DBus.ObjectManager";
const char *kBlockIface = "org.freedesktop.UDisks2.Block";
const char *kFilesystemIface = "org.freedesktop.UDisks2.Filesystem";
const char *kDriveIface = "org.freedesktop.UDisks2.Drive";

// The two nested container types returned by ObjectManager.GetManagedObjects
// (signature a{oa{sa{sv}}}): a map of object path -> (interface name ->
// properties). The inner a{sv} is QVariantMap, which QtDBus already knows;
// the two aggregate maps below are registered in the constructor.
typedef QMap<QString, QVariantMap> InterfaceProperties;
typedef QMap<QDBusObjectPath, InterfaceProperties> ManagedObjects;

// UDisks2 exposes device nodes and mount points as NUL-terminated byte arrays
// (`ay`). Decode one to a QString, dropping the trailing NUL(s).
QString byteStringToQString(const QVariant &value) {
    QByteArray bytes;
    if (value.canConvert<QByteArray>()) {
        bytes = value.toByteArray();
    } else if (value.canConvert<QDBusArgument>()) {
        value.value<QDBusArgument>() >> bytes;
    }
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromUtf8(bytes);
}

// Reads a UDisks2 `as` string-array property (e.g. Drive.MediaCompatibility)
// into one lower-cased, space-joined string for keyword matching.
QString stringArrayLower(const QVariant &value) {
    QStringList list;
    if (value.canConvert<QStringList>()) {
        list = value.toStringList();
    } else if (value.canConvert<QDBusArgument>()) {
        value.value<QDBusArgument>() >> list;
    }
    return list.join(QLatin1Char(' ')).toLower();
}

// Picks a device icon (alias into :/icons/dev-<x>.svg) from a UDisks2 Drive's
// properties, distinguishing phones, memory cards, spinning external hard
// drives and USB sticks, with a generic drive icon as the fallback.
QString iconForDrive(const QVariantMap &drive) {
    const QString bus = drive.value(QStringLiteral("ConnectionBus")).toString().toLower();
    const bool rotational = drive.value(QStringLiteral("Rotational")).toBool();
    const QString hint = (drive.value(QStringLiteral("Vendor")).toString() + QLatin1Char(' ') +
                          drive.value(QStringLiteral("Model")).toString() + QLatin1Char(' ') +
                          drive.value(QStringLiteral("Id")).toString())
                             .toLower();
    const QString media = stringArrayLower(drive.value(QStringLiteral("MediaCompatibility")));

    // Phone: recognised by model/vendor hints. Phones on MTP rarely surface as
    // block devices, but USB-storage / branded ones do. Kept specific so a
    // Samsung SSD or Apple-branded USB stick isn't mistaken for a phone.
    static const char *phoneHints[] = {"phone",  "android", "iphone", "ipad",  "mtp",
                                       "galaxy", "pixel",   "redmi",  "oneplus", "nexus"};
    for (const char *h : phoneHints) {
        if (hint.contains(QLatin1String(h)))
            return QStringLiteral("dev-phone");
    }

    // Memory card (SD / microSD / MMC / CF): by media compatibility or sd bus.
    if (bus == QLatin1String("sdio") || media.contains(QLatin1String("flash_sd")) ||
        media.contains(QLatin1String("flash_mmc")) || media.contains(QLatin1String("flash_cf")))
        return QStringLiteral("dev-sdcard");

    // Spinning platters -> a portable external hard drive.
    if (rotational)
        return QStringLiteral("dev-hdd");

    // Flash storage on the USB bus -> a USB stick (or portable SSD).
    if (bus == QLatin1String("usb"))
        return QStringLiteral("dev-usb");

    return QStringLiteral("dev-drive");
}

// Filesystem.MountPoints is an array of byte-string paths (`aay`); return the
// first non-empty one, or an empty string when the volume is not mounted.
QString firstMountPoint(const QVariant &value) {
    if (!value.canConvert<QDBusArgument>())
        return QString();
    QDBusArgument arg = value.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QByteArray entry;
        arg >> entry;
        while (entry.endsWith('\0'))
            entry.chop(1);
        if (!entry.isEmpty()) {
            arg.endArray();
            return QString::fromUtf8(entry);
        }
    }
    arg.endArray();
    return QString();
}

} // namespace

Q_DECLARE_METATYPE(InterfaceProperties)
Q_DECLARE_METATYPE(ManagedObjects)

RemovableDeviceMonitor::RemovableDeviceMonitor(QObject *parent) : QObject(parent) {
    // Teach QtDBus how to demarshal GetManagedObjects' aggregate return types.
    qDBusRegisterMetaType<InterfaceProperties>();
    qDBusRegisterMetaType<ManagedObjects>();

    // Coalesce bursts of D-Bus signals (a device emits several InterfacesAdded,
    // and UDisks emits frequent PropertiesChanged) into one settled refresh.
    m_refreshDebounce = new QTimer(this);
    m_refreshDebounce->setSingleShot(true);
    m_refreshDebounce->setInterval(200);
    connect(m_refreshDebounce, &QTimer::timeout, this, &RemovableDeviceMonitor::refresh);

    // Watch hot-plug via the ObjectManager signals. A single refresh() after
    // either signal re-reads the full state and diffs it, which naturally
    // coalesces the several InterfacesAdded a device emits (block, then
    // filesystem, then drive) into one settled snapshot. PropertiesChanged is
    // also watched so a plain unmount (device still plugged, MountPoints
    // cleared) -- which emits no InterfacesRemoved -- is noticed too.
    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        bus.connect(QString::fromUtf8(kService), QString::fromUtf8(kManagerPath),
                    QString::fromUtf8(kObjectManagerIface), QStringLiteral("InterfacesAdded"),
                    this, SLOT(handleInterfacesChanged()));
        bus.connect(QString::fromUtf8(kService), QString::fromUtf8(kManagerPath),
                    QString::fromUtf8(kObjectManagerIface), QStringLiteral("InterfacesRemoved"),
                    this, SLOT(handleInterfacesChanged()));
        // Empty object path = match PropertiesChanged from any UDisks object
        // (block/filesystem/drive); the slot just re-diffs, so over-matching is
        // harmless beyond one extra GetManagedObjects call.
        bus.connect(QString::fromUtf8(kService), QString(),
                    QStringLiteral("org.freedesktop.DBus.Properties"),
                    QStringLiteral("PropertiesChanged"), this,
                    SLOT(handleInterfacesChanged()));
    }

    m_devices = enumerate();
}

QVector<RemovableDevice> RemovableDeviceMonitor::devices() const {
    return m_devices;
}

void RemovableDeviceMonitor::handleInterfacesChanged() {
    // Debounce: a single settled refresh after the burst, not one per signal.
    m_refreshDebounce->start();
}

void RemovableDeviceMonitor::refresh() {
    const QVector<RemovableDevice> fresh = enumerate();

    // Emit deviceRemoved for every id that vanished.
    for (const RemovableDevice &old : m_devices) {
        bool stillPresent = false;
        for (const RemovableDevice &cur : fresh) {
            if (cur.id == old.id) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent)
            emit deviceRemoved(old.id);
    }

    // Emit deviceAdded for every newly-seen id, and detect any change (a new or
    // gone device, or a device whose mount state flipped) for devicesChanged().
    bool changed = fresh.size() != m_devices.size();
    for (const RemovableDevice &cur : fresh) {
        const RemovableDevice *previous = nullptr;
        for (const RemovableDevice &old : m_devices) {
            if (old.id == cur.id) {
                previous = &old;
                break;
            }
        }
        if (!previous) {
            emit deviceAdded(cur);
            changed = true;
        } else if (previous->isMounted != cur.isMounted ||
                   previous->mountPoint != cur.mountPoint ||
                   previous->name != cur.name) {
            changed = true;
        }
    }

    m_devices = fresh;
    if (changed)
        emit devicesChanged();
}

QString RemovableDeviceMonitor::ensureMounted(const QString &id, QString *errorOut) {
    // Already mounted? Hand back the known mount path without touching D-Bus.
    for (const RemovableDevice &dev : m_devices) {
        if (dev.id == id && dev.isMounted && !dev.mountPoint.isEmpty())
            return dev.mountPoint;
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        if (errorOut)
            *errorOut = tr("System D-Bus is not available");
        return QString();
    }

    QDBusInterface fs(QString::fromUtf8(kService), id, QString::fromUtf8(kFilesystemIface), bus);
    if (!fs.isValid()) {
        if (errorOut)
            *errorOut = tr("Device is not a mountable filesystem");
        return QString();
    }

    // Mount(a{sv} options) -> s: pass empty options so UDisks2 picks defaults.
    const QVariantMap options;
    QDBusReply<QString> reply = fs.call(QStringLiteral("Mount"), options);
    if (!reply.isValid()) {
        if (errorOut)
            *errorOut = reply.error().message();
        return QString();
    }

    // Refresh so the snapshot reflects the newly-mounted state.
    refresh();
    return reply.value();
}

bool RemovableDeviceMonitor::eject(const QString &id, QString *errorOut) {
    // Snapshot this device's mount state + drive path.
    bool isMounted = false;
    QString driveId;
    for (const RemovableDevice &dev : m_devices) {
        if (dev.id == id) {
            isMounted = dev.isMounted;
            driveId = dev.driveId;
            break;
        }
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        if (errorOut)
            *errorOut = tr("System D-Bus is not available");
        return false;
    }

    // 1. Unmount the filesystem first (only when it's actually mounted).
    if (isMounted) {
        QDBusInterface fs(QString::fromUtf8(kService), id, QString::fromUtf8(kFilesystemIface),
                          bus);
        if (!fs.isValid()) {
            if (errorOut)
                *errorOut = tr("Device is not a mountable filesystem");
            return false;
        }
        const QVariantMap options; // empty -> UDisks2 defaults
        QDBusReply<void> reply = fs.call(QStringLiteral("Unmount"), options);
        if (!reply.isValid()) {
            if (errorOut)
                *errorOut = reply.error().message();
            return false;
        }
    }

    // 2. Power off the drive so it's safe to physically unplug. Best-effort: some
    // drives/kernels don't support PowerOff, and a failure here after a good
    // unmount still leaves the device safely removable, so it isn't fatal.
    if (!driveId.isEmpty() && driveId != QStringLiteral("/")) {
        QDBusInterface drive(QString::fromUtf8(kService), driveId, QString::fromUtf8(kDriveIface),
                             bus);
        if (drive.isValid()) {
            const QVariantMap options;
            drive.call(QStringLiteral("PowerOff"), options);
        }
    }

    // Refresh so the snapshot reflects the now-removed/unmounted state.
    refresh();
    return true;
}

QVector<RemovableDevice> RemovableDeviceMonitor::enumerate() const {
    QVector<RemovableDevice> result;

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return result;

    QDBusInterface manager(QString::fromUtf8(kService), QString::fromUtf8(kManagerPath),
                           QString::fromUtf8(kObjectManagerIface), bus);
    if (!manager.isValid())
        return result;

    QDBusReply<ManagedObjects> reply = manager.call(QStringLiteral("GetManagedObjects"));
    if (!reply.isValid())
        return result;

    const ManagedObjects objects = reply.value();

    // First pass: index every Drive object by its path so a block's Drive
    // reference can be resolved to the properties that decide removability.
    QHash<QString, QVariantMap> drives;
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const InterfaceProperties &ifaces = it.value();
        auto driveIt = ifaces.constFind(QString::fromUtf8(kDriveIface));
        if (driveIt != ifaces.constEnd())
            drives.insert(it.key().path(), driveIt.value());
    }

    // Second pass: every block that carries a Filesystem interface and belongs
    // to a removable drive becomes a candidate device.
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const InterfaceProperties &ifaces = it.value();
        if (!ifaces.contains(QString::fromUtf8(kFilesystemIface)))
            continue;
        auto blockIt = ifaces.constFind(QString::fromUtf8(kBlockIface));
        if (blockIt == ifaces.constEnd())
            continue;

        const QVariantMap block = blockIt.value();

        // UDisks2 asks us to hide entries it flags for the drive it belongs to;
        // honour HintIgnore and skip anything marked as a system block.
        if (block.value(QStringLiteral("HintIgnore")).toBool())
            continue;
        if (block.value(QStringLiteral("HintSystem")).toBool())
            continue;

        const QString drivePath =
            block.value(QStringLiteral("Drive")).value<QDBusObjectPath>().path();
        if (drivePath.isEmpty() || drivePath == QStringLiteral("/"))
            continue;
        auto driveIt = drives.constFind(drivePath);
        if (driveIt == drives.constEnd())
            continue;
        const QVariantMap drive = driveIt.value();

        // "Removable" means the drive is user-ejectable; a USB stick may report
        // Removable=false yet be on the usb ConnectionBus, so treat either as
        // removable. Everything else (fixed system disks) is excluded.
        const bool removable = drive.value(QStringLiteral("Removable")).toBool();
        const QString connectionBus =
            drive.value(QStringLiteral("ConnectionBus")).toString();
        const bool isUsb = connectionBus == QStringLiteral("usb");
        if (!removable && !isUsb)
            continue;

        RemovableDevice dev;
        dev.id = it.key().path();
        dev.driveId = drivePath; // for safe-remove PowerOff
        dev.devNode = byteStringToQString(block.value(QStringLiteral("Device")));

        const QString label = block.value(QStringLiteral("IdLabel")).toString();
        if (!label.isEmpty()) {
            dev.name = label;
        } else {
            const QString vendor = drive.value(QStringLiteral("Vendor")).toString().trimmed();
            const QString model = drive.value(QStringLiteral("Model")).toString().trimmed();
            dev.name = (vendor + QLatin1Char(' ') + model).trimmed();
        }
        // Fall back to the device node so an entry is never nameless.
        if (dev.name.isEmpty())
            dev.name = dev.devNode;

        const QVariantMap filesystem = ifaces.value(QString::fromUtf8(kFilesystemIface));
        dev.mountPoint = firstMountPoint(filesystem.value(QStringLiteral("MountPoints")));
        dev.isMounted = !dev.mountPoint.isEmpty();

        dev.iconName = iconForDrive(drive);

        result.append(dev);
    }

    return result;
}
