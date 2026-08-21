#include "PendingSendQueue.h"

#include <QSet>

PendingSendQueue::PendingSendQueue(QObject *parent) : QObject(parent) {}

void PendingSendQueue::enqueue(const QString &deviceId, const QString &name,
                               const QStringList &sources) {
    // One entry per (device, sources) request, not per device: the same device
    // can be sent several selections while offline and each must fire on its own
    // once it is reachable.
    m_entries.append({deviceId, name, sources});
    emit queued(deviceId);
}

void PendingSendQueue::devicesChanged(const QStringList &onlineIds) {
    if (m_entries.isEmpty())
        return;
    const QSet<QString> online(onlineIds.begin(), onlineIds.end());
    QVector<Entry> stillPending;
    stillPending.reserve(m_entries.size());
    for (const Entry &entry : qAsConst(m_entries)) {
        if (online.contains(entry.deviceId))
            emit sendReady(entry.deviceId, entry.name, entry.sources);
        else
            stillPending.append(entry);
    }
    m_entries = stillPending;
}
