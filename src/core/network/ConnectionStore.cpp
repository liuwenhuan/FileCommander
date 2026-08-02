#include "ConnectionStore.h"

#include <QSettings>
#include <QUuid>

#include "CredentialStore.h"
#include "config/Settings.h"

namespace {

// Bookmarks live in the same INI as the rest of the configuration (shared path
// via Settings, so there's one source of truth for where config lives).
QSettings settings() {
    return QSettings(Settings::configFilePath(), QSettings::IniFormat);
}

SavedConnection readGroup(QSettings &s, const QString &id) {
    SavedConnection c;
    c.id = id;
    s.beginGroup(QStringLiteral("connections/") + id);
    c.name = s.value(QStringLiteral("name")).toString();
    c.protocol = s.value(QStringLiteral("protocol"), 0).toInt();
    c.host = s.value(QStringLiteral("host")).toString();
    c.port = s.value(QStringLiteral("port"), 0).toInt();
    c.user = s.value(QStringLiteral("user")).toString();
    c.remotePath = s.value(QStringLiteral("remotePath"), QStringLiteral("/")).toString();
    c.anonymous = s.value(QStringLiteral("anonymous"), false).toBool();
    c.created = QDateTime::fromString(s.value(QStringLiteral("created")).toString(),
                                      Qt::ISODate);
    s.endGroup();
    return c;
}

} // namespace

QVector<SavedConnection> ConnectionStore::loadAll() {
    QVector<SavedConnection> result;
    QSettings s = settings();
    // Ordered id list preserves the order bookmarks were saved in.
    const QStringList ids = s.value(QStringLiteral("connections/order")).toStringList();
    result.reserve(ids.size());
    for (const QString &id : ids) {
        if (s.contains(QStringLiteral("connections/") + id + QStringLiteral("/host")))
            result.append(readGroup(s, id));
    }
    return result;
}

SavedConnection ConnectionStore::load(const QString &id) {
    if (id.isEmpty())
        return {};
    QSettings s = settings();
    if (!s.contains(QStringLiteral("connections/") + id + QStringLiteral("/host")))
        return {};
    return readGroup(s, id);
}

QString ConnectionStore::save(const SavedConnection &conn) {
    SavedConnection c = conn;
    if (c.id.isEmpty())
        c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSettings s = settings();
    // Creation time is written once and then left alone: an edit re-saves the
    // whole group, so taking `conn.created` unconditionally would let any caller
    // that built a SavedConnection from scratch (the reconnect bookkeeping does
    // exactly that) silently reset it.
    const QString createdKey = QStringLiteral("connections/") + c.id + QStringLiteral("/created");
    const QString existingCreated = s.value(createdKey).toString();
    if (existingCreated.isEmpty())
        c.created = QDateTime::currentDateTime();
    else
        c.created = QDateTime::fromString(existingCreated, Qt::ISODate);

    s.beginGroup(QStringLiteral("connections/") + c.id);
    s.setValue(QStringLiteral("created"), c.created.toString(Qt::ISODate));
    s.setValue(QStringLiteral("name"), c.name);
    s.setValue(QStringLiteral("protocol"), c.protocol);
    s.setValue(QStringLiteral("host"), c.host);
    s.setValue(QStringLiteral("port"), c.port);
    s.setValue(QStringLiteral("user"), c.user);
    s.setValue(QStringLiteral("remotePath"), c.remotePath);
    s.setValue(QStringLiteral("anonymous"), c.anonymous);
    s.endGroup();

    // Append to the ordered id list on first save.
    QStringList order = s.value(QStringLiteral("connections/order")).toStringList();
    if (!order.contains(c.id)) {
        order.append(c.id);
        s.setValue(QStringLiteral("connections/order"), order);
    }
    return c.id;
}

void ConnectionStore::remove(const QString &id) {
    if (id.isEmpty())
        return;
    QSettings s = settings();
    s.remove(QStringLiteral("connections/") + id);
    QStringList order = s.value(QStringLiteral("connections/order")).toStringList();
    if (order.removeAll(id) > 0)
        s.setValue(QStringLiteral("connections/order"), order);
    clearPassword(id);
}

bool ConnectionStore::storePassword(const QString &id, const QString &password) {
    return CredentialStore::save(id, password).ok;
}

QString ConnectionStore::loadPassword(const QString &id) {
    QString password;
    CredentialStore::load(id, &password);
    return password;
}

void ConnectionStore::clearPassword(const QString &id) {
    CredentialStore::remove(id);
}
