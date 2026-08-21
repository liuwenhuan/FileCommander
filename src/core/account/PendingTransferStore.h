#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// One unfinished device send: the peer it was going to, and the local files
// that were on their way.
struct PendingSend {
    QString deviceId;
    QString deviceName;
    QStringList sources;
};

// Persistent record of device sends that did not finish, so a launch after a
// crash or a dropped link can offer to resume them instead of leaving the user
// to re-drag every file. One JSON array at <configDir>/pending-transfers.json.
// Deliberately small and self-healing: a missing file is an empty list and a
// corrupt one is treated as empty rather than crashing the launch.
//
// This is the "remember and re-offer" half of transfer history, not a full
// history of completed transfers: a send is recorded when it starts and removed
// when it is reported done, so what survives a crash is exactly what needs
// resuming.
class PendingTransferStore {
public:
    PendingTransferStore(); // uses Settings::configDir()

    QVector<PendingSend> entries() const;
    // Records a send; an identical entry already present is not duplicated.
    void add(const QString &deviceId, const QString &deviceName, const QStringList &sources);
    // Removes the entry that matches deviceId + sources exactly (a fresh send
    // or a resume that completed).
    void remove(const QString &deviceId, const QStringList &sources);
    void clear();

private:
    QString filePath() const;
    void write(const QVector<PendingSend> &entries) const;
};
