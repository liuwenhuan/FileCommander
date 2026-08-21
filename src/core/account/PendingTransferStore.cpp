#include "PendingTransferStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "config/Settings.h"

namespace {

QString sourcesKey(const QStringList &sources) {
    return sources.join(QLatin1Char('\n'));
}

} // namespace

PendingTransferStore::PendingTransferStore() {
    // Nothing to hold: the config dir is resolved per call, so a store
    // constructed before the config dir exists still works once it does.
}

QString PendingTransferStore::filePath() const {
    const QString dir = Settings::configDir();
    if (dir.isEmpty())
        return QString();
    return dir + QStringLiteral("/pending-transfers.json");
}

QVector<PendingSend> PendingTransferStore::entries() const {
    const QString path = filePath();
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {}; // missing file == no pending sends
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return {}; // corrupt == treat as empty, don't crash the launch
    QVector<PendingSend> result;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject obj = value.toObject();
        PendingSend send;
        send.deviceId = obj.value(QStringLiteral("deviceId")).toString();
        send.deviceName = obj.value(QStringLiteral("deviceName")).toString();
        for (const QJsonValue &s : obj.value(QStringLiteral("sources")).toArray())
            send.sources.append(s.toString());
        if (!send.deviceId.isEmpty() && !send.sources.isEmpty())
            result.append(send);
    }
    return result;
}

void PendingTransferStore::write(const QVector<PendingSend> &entries) const {
    const QString path = filePath();
    if (path.isEmpty())
        return;
    QJsonArray array;
    for (const PendingSend &send : entries) {
        QJsonObject obj;
        obj.insert(QStringLiteral("deviceId"), send.deviceId);
        obj.insert(QStringLiteral("deviceName"), send.deviceName);
        QJsonArray sources;
        for (const QString &s : send.sources)
            sources.append(s);
        obj.insert(QStringLiteral("sources"), sources);
        array.append(obj);
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

void PendingTransferStore::add(const QString &deviceId, const QString &deviceName,
                               const QStringList &sources) {
    QVector<PendingSend> list = entries();
    for (const PendingSend &existing : list) {
        if (existing.deviceId == deviceId && existing.sources == sources)
            return; // already recorded
    }
    PendingSend send;
    send.deviceId = deviceId;
    send.deviceName = deviceName;
    send.sources = sources;
    list.append(send);
    write(list);
}

void PendingTransferStore::remove(const QString &deviceId, const QStringList &sources) {
    QVector<PendingSend> list = entries();
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).deviceId == deviceId && list.at(i).sources == sources) {
            list.removeAt(i);
            write(list);
            return;
        }
    }
}

void PendingTransferStore::clear() {
    write({});
}
