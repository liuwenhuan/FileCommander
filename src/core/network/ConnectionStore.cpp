#include "ConnectionStore.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

// libsecret pulls in glib/gio headers that use `signals`/`slots` as ordinary
// identifiers, which collide with Qt's keyword macros. This translation unit
// has no Q_OBJECT, so undefining them for the libsecret include is harmless.
#undef signals
#undef slots
#include <libsecret/secret.h>

namespace {

// Same INI file Settings writes to, so bookmarks live alongside the rest of the
// configuration.
QString configFilePath() {
    const QString configDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        QStringLiteral("/totalcommander");
    QDir().mkpath(configDir);
    return configDir + QStringLiteral("/config.ini");
}

QSettings settings() {
    return QSettings(configFilePath(), QSettings::IniFormat);
}

// libsecret schema for our passwords. A single "id" attribute keys each secret
// to its bookmark. SECRET_SCHEMA_NONE keeps libsecret from imposing extra
// attribute matching rules.
const SecretSchema *passwordSchema() {
    static const SecretSchema schema = {
        "org.ttc.Connection",
        SECRET_SCHEMA_NONE,
        {
            {"id", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
        // Reserved trailing fields.
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    return &schema;
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
    s.beginGroup(QStringLiteral("connections/") + c.id);
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
    if (id.isEmpty())
        return false;
    GError *error = nullptr;
    const QString label = QStringLiteral("ttc connection %1").arg(id);
    const bool ok = secret_password_store_sync(
        passwordSchema(), SECRET_COLLECTION_DEFAULT, label.toUtf8().constData(),
        password.toUtf8().constData(), nullptr /*cancellable*/, &error, "id",
        id.toUtf8().constData(), nullptr);
    if (error) {
        g_error_free(error);
        return false;
    }
    return ok;
}

QString ConnectionStore::loadPassword(const QString &id) {
    if (id.isEmpty())
        return QString();
    GError *error = nullptr;
    gchar *secret = secret_password_lookup_sync(passwordSchema(), nullptr, &error, "id",
                                                id.toUtf8().constData(), nullptr);
    if (error) {
        g_error_free(error);
        return QString();
    }
    if (!secret)
        return QString();
    const QString password = QString::fromUtf8(secret);
    secret_password_free(secret);
    return password;
}

void ConnectionStore::clearPassword(const QString &id) {
    if (id.isEmpty())
        return;
    GError *error = nullptr;
    secret_password_clear_sync(passwordSchema(), nullptr, &error, "id",
                               id.toUtf8().constData(), nullptr);
    if (error)
        g_error_free(error);
}
