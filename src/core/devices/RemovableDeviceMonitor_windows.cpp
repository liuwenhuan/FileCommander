#include "RemovableDeviceMonitor.h"
#include "WindowsRemovableStorage.h"

#include <QByteArray>
#include <QDir>
#include <QStorageInfo>
#include <QTimer>

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace {

bool isExternalBus(STORAGE_BUS_TYPE busType) {
    return busType == BusTypeUsb || busType == BusType1394 || busType == BusTypeSd ||
           busType == BusTypeMmc;
}

bool queryPhysicalDriveRemovability(DWORD diskNumber, bool *deviceHotplug,
                                    bool *mediaRemovable) {
    const QString devicePath = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskNumber);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    bool queried = false;
    DWORD bytesReturned = 0;
    STORAGE_HOTPLUG_INFO hotplugInfo{};
    hotplugInfo.Size = sizeof(hotplugInfo);
    if (DeviceIoControl(handle, IOCTL_STORAGE_GET_HOTPLUG_INFO, nullptr, 0, &hotplugInfo,
                        sizeof(hotplugInfo), &bytesReturned, nullptr)) {
        *deviceHotplug = *deviceHotplug || hotplugInfo.DeviceHotplug;
        *mediaRemovable = *mediaRemovable || hotplugInfo.MediaRemovable;
        queried = true;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    QByteArray descriptorBuffer(sizeof(STORAGE_DEVICE_DESCRIPTOR) + 256, '\0');
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        descriptorBuffer.data(), static_cast<DWORD>(descriptorBuffer.size()),
                        &bytesReturned, nullptr) &&
        bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto *descriptor =
            reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(descriptorBuffer.constData());
        *deviceHotplug = *deviceHotplug || isExternalBus(descriptor->BusType);
        queried = true;
    }

    CloseHandle(handle);
    return queried;
}

bool queryVolumeRemovability(const QString &rootPath, bool *deviceHotplug,
                             bool *mediaRemovable) {
    if (rootPath.size() < 2 || rootPath.at(1) != QLatin1Char(':'))
        return false;

    const QString volumePath = QStringLiteral("\\\\.\\%1:").arg(rootPath.left(1));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(volumePath.utf16()), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    QByteArray extentsBuffer(sizeof(VOLUME_DISK_EXTENTS) + 15 * sizeof(DISK_EXTENT), '\0');
    DWORD bytesReturned = 0;
    const bool gotExtents = DeviceIoControl(
        handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, extentsBuffer.data(),
        static_cast<DWORD>(extentsBuffer.size()), &bytesReturned, nullptr);
    CloseHandle(handle);
    if (!gotExtents || bytesReturned < sizeof(VOLUME_DISK_EXTENTS))
        return false;

    const auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(extentsBuffer.constData());
    const DWORD maxExtents = static_cast<DWORD>(
        (extentsBuffer.size() - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT));
    const DWORD extentCount = qMin(extents->NumberOfDiskExtents, maxExtents);
    bool queried = false;
    for (DWORD index = 0; index < extentCount; ++index) {
        queried = queryPhysicalDriveRemovability(extents->Extents[index].DiskNumber,
                                                  deviceHotplug, mediaRemovable) ||
                  queried;
    }
    return queried;
}

QString windowsError(DWORD code) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    const QString result = length ? QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed()
                                  : QStringLiteral("Windows error %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return result;
}

} // namespace

RemovableDeviceMonitor::RemovableDeviceMonitor(QObject *parent) : QObject(parent) {
    m_refreshDebounce = new QTimer(this);
    m_refreshDebounce->setInterval(1000);
    connect(m_refreshDebounce, &QTimer::timeout, this, &RemovableDeviceMonitor::refresh);
    m_refreshDebounce->start();
    refresh();
}

QVector<RemovableDevice> RemovableDeviceMonitor::devices() const { return m_devices; }

QVector<RemovableDevice> RemovableDeviceMonitor::enumerate() const {
    QVector<RemovableDevice> result;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || volume.isRoot())
            continue;

        const QString rootPath = QDir::toNativeSeparators(volume.rootPath());
        const UINT driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(rootPath.utf16()));
        bool deviceHotplug = false;
        bool mediaRemovable = false;
        queryVolumeRemovability(rootPath, &deviceHotplug, &mediaRemovable);
        if (!WindowsRemovableStorage::shouldExposeAsRemovable(driveType, deviceHotplug,
                                                              mediaRemovable))
            continue;

        RemovableDevice dev;
        dev.id = rootPath;
        dev.mountPoint = rootPath;
        dev.devNode = QString::fromLocal8Bit(volume.device());
        dev.name = volume.displayName();
        if (dev.name.isEmpty())
            dev.name = dev.mountPoint;
        dev.iconName = QStringLiteral("dev-drive");
        dev.isMounted = true;
        result.append(dev);
    }
    return result;
}

void RemovableDeviceMonitor::refresh() {
    const QVector<RemovableDevice> fresh = enumerate();
    for (const RemovableDevice &dev : fresh) {
        bool found = false;
        for (const RemovableDevice &old : m_devices)
            found = found || old.id.compare(dev.id, Qt::CaseInsensitive) == 0;
        if (!found)
            emit deviceAdded(dev);
    }
    for (const RemovableDevice &old : m_devices) {
        bool found = false;
        for (const RemovableDevice &dev : fresh)
            found = found || old.id.compare(dev.id, Qt::CaseInsensitive) == 0;
        if (!found)
            emit deviceRemoved(old.id);
    }
    bool changed = fresh.size() != m_devices.size();
    for (int i = 0; !changed && i < fresh.size(); ++i)
        changed = fresh.at(i).id.compare(m_devices.at(i).id, Qt::CaseInsensitive) != 0 ||
                  fresh.at(i).mountPoint.compare(m_devices.at(i).mountPoint,
                                                 Qt::CaseInsensitive) != 0;
    if (changed) {
        m_devices = fresh;
        emit devicesChanged();
    }
}

QString RemovableDeviceMonitor::ensureMounted(const QString &id, QString *errorOut) {
    for (const RemovableDevice &dev : m_devices)
        if (dev.id.compare(id, Qt::CaseInsensitive) == 0)
            return dev.mountPoint;
    if (errorOut)
        *errorOut = tr("The volume is no longer available.");
    return {};
}

bool RemovableDeviceMonitor::eject(const QString &id, QString *errorOut) {
    const QString root = QDir::toNativeSeparators(id);
    if (root.size() < 2 || root.at(1) != QLatin1Char(':')) {
        if (errorOut)
            *errorOut = tr("The volume is no longer available.");
        return false;
    }
    const QString devicePath = QStringLiteral("\\\\.\\%1:").arg(root.left(1));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (errorOut)
            *errorOut = windowsError(GetLastError());
        return false;
    }

    DWORD ignored = 0;
    if (!DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr) ||
        !DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr)) {
        const QString error = windowsError(GetLastError());
        DeviceIoControl(handle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr);
        CloseHandle(handle);
        if (errorOut)
            *errorOut = error;
        return false;
    }

    // Some USB enclosures do not implement a physical media-eject ioctl. A
    // locked, dismounted volume is nevertheless safe to unplug, so success is
    // determined by the lock/dismount pair above.
    DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &ignored, nullptr);
    CloseHandle(handle);
    refresh();
    return true;
}

void RemovableDeviceMonitor::handleInterfacesChanged() { refresh(); }
