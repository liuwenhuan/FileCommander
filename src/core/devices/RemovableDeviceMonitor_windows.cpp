#include "RemovableDeviceMonitor.h"

#include <QStorageInfo>
#include <QTimer>

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
        RemovableDevice dev;
        dev.id = volume.rootPath();
        dev.mountPoint = volume.rootPath();
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

bool RemovableDeviceMonitor::eject(const QString &, QString *errorOut) {
    if (errorOut)
        *errorOut = tr("Safe removal is not available in this build.");
    return false;
}

void RemovableDeviceMonitor::handleInterfacesChanged() { refresh(); }
