#include "RemovableDeviceMonitor.h"

#import <AppKit/AppKit.h>
#import <DiskArbitration/DiskArbitration.h>

#include <dispatch/dispatch.h>

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimer>

#include <limits.h>

namespace {

struct MacDeviceState {
    NSNotificationCenter *notificationCenter = nil;
    id mountObserver = nil;
    id unmountObserver = nil;
    id renameObserver = nil;
};

QString fromCFString(CFStringRef value) {
    if (!value)
        return {};
    char buffer[PATH_MAX] = {};
    if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8))
        return QString::fromUtf8(buffer);
    return {};
}

bool dictionaryBool(CFDictionaryRef dictionary, const void *key) {
    const auto value =
        static_cast<CFBooleanRef>(CFDictionaryGetValue(dictionary, key));
    return value && CFGetTypeID(value) == CFBooleanGetTypeID() &&
           CFBooleanGetValue(value);
}

CFTypeRef dictionaryValue(CFDictionaryRef dictionary, const void *key) {
    return dictionary ? CFDictionaryGetValue(dictionary, key) : nullptr;
}

QString diskError(DADissenterRef dissenter, const QString &fallback) {
    if (dissenter) {
        if (CFStringRef text = DADissenterGetStatusString(dissenter)) {
            const QString message = fromCFString(text);
            if (!message.isEmpty())
                return message;
        }
        return QStringLiteral("Disk Arbitration rejected the operation.");
    }
    return fallback;
}

QString normalizedBSDName(const QString &id) {
    QString name = id.trimmed();
    if (name.startsWith(QStringLiteral("/dev/")))
        name.remove(0, 5);
    return name;
}

struct DiskOperation {
    dispatch_semaphore_t finished = nullptr;
    QString mountPoint;
    QString error;
    bool success = false;
};

void mountCallback(DADiskRef disk, DADissenterRef dissenter, void *context) {
    auto *operation = static_cast<DiskOperation *>(context);
    if (!dissenter && disk) {
        CFDictionaryRef description = DADiskCopyDescription(disk);
        if (description) {
            if (auto path = static_cast<CFURLRef>(
                    dictionaryValue(description, kDADiskDescriptionVolumePathKey))) {
                char buffer[PATH_MAX] = {};
                if (CFURLGetFileSystemRepresentation(path, true, reinterpret_cast<UInt8 *>(buffer),
                                                      sizeof(buffer)))
                    operation->mountPoint = QString::fromUtf8(buffer);
            }
            CFRelease(description);
        }
        operation->success = !operation->mountPoint.isEmpty();
    } else {
        operation->error = diskError(dissenter, QStringLiteral("Could not mount the volume."));
    }
    dispatch_semaphore_signal(operation->finished);
}

void ejectCallback(DADiskRef, DADissenterRef dissenter, void *context) {
    auto *operation = static_cast<DiskOperation *>(context);
    operation->success = dissenter == nullptr;
    if (dissenter)
        operation->error = diskError(dissenter, QStringLiteral("Could not eject the volume."));
    dispatch_semaphore_signal(operation->finished);
}

bool isExternalVolume(CFDictionaryRef description) {
    if (!description)
        return false;
    if (dictionaryBool(description, kDADiskDescriptionVolumeNetworkKey))
        return false;

    const bool removable =
        dictionaryBool(description, kDADiskDescriptionMediaRemovableKey);
    const bool ejectable =
        dictionaryBool(description, kDADiskDescriptionMediaEjectableKey);
    const auto internal =
        static_cast<CFBooleanRef>(dictionaryValue(description,
                                                   kDADiskDescriptionDeviceInternalKey));
    const bool isInternal = internal && CFGetTypeID(internal) == CFBooleanGetTypeID() &&
                            CFBooleanGetValue(internal);
    const QString protocol = fromCFString(static_cast<CFStringRef>(
                                               dictionaryValue(description,
                                                               kDADiskDescriptionDeviceProtocolKey)))
                                 .toLower();

    if (removable || ejectable)
        return true;
    if (isInternal)
        return false;
    return protocol.contains(QStringLiteral("usb")) ||
           protocol.contains(QStringLiteral("firewire")) ||
           protocol.contains(QStringLiteral("thunderbolt")) ||
           protocol.contains(QStringLiteral("sd"));
}

QString iconForDescription(CFDictionaryRef description) {
    const QString protocol = fromCFString(static_cast<CFStringRef>(
                                               dictionaryValue(description,
                                                               kDADiskDescriptionDeviceProtocolKey)))
                                 .toLower();
    if (protocol.contains(QStringLiteral("sd")))
        return QStringLiteral("dev-sdcard");
    if (protocol.contains(QStringLiteral("usb")))
        return QStringLiteral("dev-usb");
    if (protocol.contains(QStringLiteral("firewire")) ||
        protocol.contains(QStringLiteral("thunderbolt")))
        return QStringLiteral("dev-hdd");
    return QStringLiteral("dev-drive");
}

} // namespace

RemovableDeviceMonitor::RemovableDeviceMonitor(QObject *parent) : QObject(parent) {
    auto *state = new MacDeviceState;
    state->notificationCenter = [NSWorkspace sharedWorkspace].notificationCenter;
    const auto scheduleRefresh = [this] {
        QMetaObject::invokeMethod(this, "handleInterfacesChanged", Qt::QueuedConnection);
    };
    state->mountObserver =
        [state->notificationCenter addObserverForName:NSWorkspaceDidMountNotification
                                                object:nil
                                                 queue:[NSOperationQueue mainQueue]
                                            usingBlock:^(NSNotification *) {
                                                scheduleRefresh();
                                            }];
    state->unmountObserver =
        [state->notificationCenter addObserverForName:NSWorkspaceDidUnmountNotification
                                                object:nil
                                                 queue:[NSOperationQueue mainQueue]
                                            usingBlock:^(NSNotification *) {
                                                scheduleRefresh();
                                            }];
    state->renameObserver =
        [state->notificationCenter
            addObserverForName:NSWorkspaceDidRenameVolumeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *) {
                        scheduleRefresh();
                    }];
    m_nativeState = state;

    // The workspace notifications are immediate. The settled snapshot timer
    // catches a volume whose mount notification races Disk Arbitration's final
    // QStorageInfo state.
    m_refreshDebounce = new QTimer(this);
    m_refreshDebounce->setInterval(1000);
    connect(m_refreshDebounce, &QTimer::timeout, this, &RemovableDeviceMonitor::refresh);
    m_refreshDebounce->start();
    m_devices = enumerate();
}

RemovableDeviceMonitor::~RemovableDeviceMonitor() {
    auto *state = static_cast<MacDeviceState *>(m_nativeState);
    if (!state)
        return;
    m_nativeState = nullptr;
    if (state->notificationCenter) {
        if (state->mountObserver)
            [state->notificationCenter removeObserver:state->mountObserver];
        if (state->unmountObserver)
            [state->notificationCenter removeObserver:state->unmountObserver];
        if (state->renameObserver)
            [state->notificationCenter removeObserver:state->renameObserver];
    }
    delete state;
}

QVector<RemovableDevice> RemovableDeviceMonitor::devices() const {
    return m_devices;
}

void RemovableDeviceMonitor::handleInterfacesChanged() {
    refresh();
}

void RemovableDeviceMonitor::refresh() {
    const QVector<RemovableDevice> fresh = enumerate();

    for (const RemovableDevice &current : fresh) {
        bool present = false;
        for (const RemovableDevice &old : m_devices) {
            if (old.id == current.id) {
                present = true;
                break;
            }
        }
        if (!present)
            emit deviceAdded(current);
    }
    for (const RemovableDevice &old : m_devices) {
        bool present = false;
        for (const RemovableDevice &current : fresh) {
            if (old.id == current.id) {
                present = true;
                break;
            }
        }
        if (!present)
            emit deviceRemoved(old.id);
    }

    bool changed = fresh.size() != m_devices.size();
    for (const RemovableDevice &current : fresh) {
        for (const RemovableDevice &old : m_devices) {
            if (old.id != current.id)
                continue;
            changed = changed || old.mountPoint != current.mountPoint ||
                      old.name != current.name ||
                      old.bytesTotal != current.bytesTotal ||
                      old.bytesAvailable != current.bytesAvailable;
            break;
        }
    }

    if (changed) {
        m_devices = fresh;
        emit devicesChanged();
    }
}

QString RemovableDeviceMonitor::ensureMounted(const QString &id, QString *errorOut) {
    for (const RemovableDevice &device : m_devices) {
        if (device.id == id && device.isMounted && !device.mountPoint.isEmpty())
            return device.mountPoint;
    }

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) {
        if (errorOut)
            *errorOut = tr("Disk Arbitration is not available.");
        return {};
    }
    DASessionSetDispatchQueue(session,
                              dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));

    const QByteArray bsdName = normalizedBSDName(id).toLocal8Bit();
    DADiskRef disk =
        DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName.constData());
    if (!disk) {
        CFRelease(session);
        if (errorOut)
            *errorOut = tr("The volume is no longer available.");
        return {};
    }

    DiskOperation operation;
    operation.finished = dispatch_semaphore_create(0);
    DADiskMount(disk, nullptr, kDADiskMountOptionDefault, mountCallback, &operation);
    dispatch_semaphore_wait(operation.finished, DISPATCH_TIME_FOREVER);
    const QString mountPoint = operation.mountPoint;
    const QString error = operation.error;
    CFRelease(disk);
    CFRelease(session);

    if (!operation.success) {
        if (errorOut)
            *errorOut = error.isEmpty() ? tr("Could not mount the volume.") : error;
        return {};
    }
    refresh();
    return mountPoint;
}

bool RemovableDeviceMonitor::eject(const QString &id, QString *errorOut) {
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) {
        if (errorOut)
            *errorOut = tr("Disk Arbitration is not available.");
        return false;
    }
    DASessionSetDispatchQueue(session,
                              dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));

    const QByteArray bsdName = normalizedBSDName(id).toLocal8Bit();
    DADiskRef disk =
        DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName.constData());
    if (!disk) {
        CFRelease(session);
        if (errorOut)
            *errorOut = tr("The volume is no longer available.");
        return false;
    }
    DADiskRef wholeDisk = DADiskCopyWholeDisk(disk);
    if (!wholeDisk)
        wholeDisk = disk;

    DiskOperation operation;
    operation.finished = dispatch_semaphore_create(0);
    DADiskEject(wholeDisk, kDADiskEjectOptionDefault, ejectCallback, &operation);
    dispatch_semaphore_wait(operation.finished, DISPATCH_TIME_FOREVER);
    const QString error = operation.error;
    const bool success = operation.success;
    if (wholeDisk != disk)
        CFRelease(wholeDisk);
    CFRelease(disk);
    CFRelease(session);

    if (!success) {
        if (errorOut)
            *errorOut = error.isEmpty() ? tr("Could not eject the volume.") : error;
        return false;
    }
    refresh();
    return true;
}

QVector<RemovableDevice> RemovableDeviceMonitor::enumerate() const {
    QVector<RemovableDevice> result;
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session)
        return result;

    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady() || storage.isRoot())
            continue;

        const QString mountPoint = QDir::cleanPath(storage.rootPath());
        if (mountPoint.startsWith(QStringLiteral("/System/Volumes/")))
            continue;

        const QByteArray device = storage.device();
        if (device.isEmpty())
            continue;
        QString id = QString::fromLocal8Bit(device);
        if (!id.startsWith(QStringLiteral("/dev/")))
            id.prepend(QStringLiteral("/dev/"));
        const QByteArray bsdName = id.mid(5).toLocal8Bit();
        DADiskRef disk =
            DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName.constData());
        if (!disk)
            continue;
        CFDictionaryRef description = DADiskCopyDescription(disk);
        if (!isExternalVolume(description)) {
            if (description)
                CFRelease(description);
            CFRelease(disk);
            continue;
        }

        RemovableDevice deviceInfo;
        deviceInfo.id = id;
        deviceInfo.devNode = id;
        deviceInfo.mountPoint = mountPoint;
        deviceInfo.isMounted = true;
        deviceInfo.name = storage.displayName();
        if (deviceInfo.name.isEmpty())
            deviceInfo.name = QFileInfo(mountPoint).fileName();
        if (deviceInfo.name.isEmpty())
            deviceInfo.name = id;
        deviceInfo.iconName = iconForDescription(description);
        deviceInfo.bytesTotal = storage.bytesTotal();
        deviceInfo.bytesAvailable = storage.bytesAvailable();
        result.append(deviceInfo);

        if (description)
            CFRelease(description);
        CFRelease(disk);
    }
    CFRelease(session);
    return result;
}
