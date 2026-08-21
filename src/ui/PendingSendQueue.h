#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// In-memory queue of "send these files to that device" requests made while the
// device was offline. The device's presence comes from AccountClient, so a send
// queued for an offline device waits here until that device next shows as
// online, then drains.
//
// Deliberately session-only: nothing is persisted, so a queued send does not
// survive an app exit. That ceiling is the whole feature for now -- persisting
// it would drag in a reconcile-with-disk problem (did the file change since it
// was queued? is the destination still wanted?) that is not worth it for a
// convenience feature.
class PendingSendQueue : public QObject {
    Q_OBJECT

public:
    struct Entry {
        QString deviceId;
        QString name;
        QStringList sources;
    };

    explicit PendingSendQueue(QObject *parent = nullptr);

    // Queues a send for a device that is currently offline. Emits queued() so
    // the caller can tell the user it is waiting.
    void enqueue(const QString &deviceId, const QString &name, const QStringList &sources);

    // Feeds the ids of the devices that are online right now. Every queued send
    // whose device is among them is drained and emitted as sendReady() -- the
    // caller wires that to its actual send path.
    void devicesChanged(const QStringList &onlineIds);

    int pendingCount() const { return m_entries.size(); }

signals:
    void queued(const QString &deviceId);
    void sendReady(const QString &deviceId, const QString &name, const QStringList &sources);

private:
    QVector<Entry> m_entries;
};
