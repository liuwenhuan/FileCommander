#pragma once

#include <QObject>
#include <QString>
#include <QVector>

// A single removable storage volume as reported by UDisks2: a USB stick, an
// external hard drive, a phone's mass-storage partition, etc. The `id` is the
// UDisks2 object path of the block device, which is stable for the lifetime of
// the device and doubles as the key passed back to ensureMounted().
struct RemovableDevice {
    QString id;          // stable key: UDisks object path, e.g.
                         // /org/freedesktop/UDisks2/block_devices/sdb1
    QString name;        // display name: volume label (IdLabel), or the drive's
                         // "Vendor Model" when the volume has no label
    QString mountPoint;  // filesystem mount path when mounted, otherwise empty
    QString devNode;     // /dev/sdb1
    QString iconName;    // device-class icon alias: "dev-usb" (USB stick),
                         // "dev-hdd" (external hard drive), "dev-phone" (phone),
                         // "dev-sdcard" (memory card) or "dev-drive" (generic);
                         // maps to :/icons/<iconName>.svg
    bool isMounted = false;
};

// Enumerates and watches removable storage devices through UDisks2
// (org.freedesktop.UDisks2) on the D-Bus system bus. Hot-plug of a device is
// reported via deviceAdded/deviceRemoved, with devicesChanged() emitted for any
// change (including a device becoming mounted/unmounted).
//
// When UDisks2 is unavailable (service not running, no system bus) the monitor
// degrades gracefully: devices() stays empty and no signals fire -- it never
// throws or crashes.
class RemovableDeviceMonitor : public QObject {
    Q_OBJECT
public:
    explicit RemovableDeviceMonitor(QObject *parent = nullptr);

    // Current snapshot of removable devices.
    QVector<RemovableDevice> devices() const;

    // Re-enumerate from UDisks2 and emit the appropriate change signals.
    void refresh();

    // Ensures the device with this id is mounted, mounting it via UDisks2 if
    // needed, and returns the mount path. Returns an already-mounted device's
    // existing mount path immediately. On failure returns an empty string and,
    // when errorOut is non-null, writes a human-readable reason to it.
    QString ensureMounted(const QString &id, QString *errorOut = nullptr);

signals:
    void deviceAdded(const RemovableDevice &dev);
    void deviceRemoved(const QString &id);
    void devicesChanged();

private slots:
    // UDisks2 InterfacesAdded/InterfacesRemoved land here; both simply trigger a
    // refresh which diffs the snapshot and emits the per-device signals.
    void handleInterfacesChanged();

private:
    // Full enumeration via ObjectManager.GetManagedObjects; returns an empty
    // vector when UDisks2 can't be reached.
    QVector<RemovableDevice> enumerate() const;

    QVector<RemovableDevice> m_devices;
};
