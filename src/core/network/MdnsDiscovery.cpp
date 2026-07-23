#include "MdnsDiscovery.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QList>
#include <QVariant>

namespace {

// Avahi D-Bus names.
const char *kService = "org.freedesktop.Avahi";
const char *kServerPath = "/";
const char *kServerIface = "org.freedesktop.Avahi.Server";
const char *kBrowserIface = "org.freedesktop.Avahi.ServiceBrowser";
const char *kResolverIface = "org.freedesktop.Avahi.ServiceResolver";

// The DNS-SD service type advertised by SMB/CIFS shares (NAS boxes, macOS,
// Samba with avahi). Browsing all interfaces / both address families.
const char *kSmbType = "_smb._tcp";

// AVAHI_IF_UNSPEC / AVAHI_PROTO_UNSPEC: every interface, IPv4 and IPv6.
const int kIfaceUnspec = -1;
const int kProtoUnspec = -1;

} // namespace

MdnsDiscovery::MdnsDiscovery(QObject *parent) : QObject(parent) {}

MdnsDiscovery::~MdnsDiscovery() {
    stop();
}

void MdnsDiscovery::start() {
    m_finished = false;

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        emitFinishedOnce();
        return;
    }

    // ServiceBrowserNew(interface, protocol, type, domain, flags) -> object path.
    // Passing the arguments as int/uint QVariants marshals them to the i/u
    // D-Bus types Avahi expects.
    QDBusInterface server(QString::fromUtf8(kService), QString::fromUtf8(kServerPath),
                          QString::fromUtf8(kServerIface), bus);
    if (!server.isValid()) {
        emitFinishedOnce();
        return;
    }

    QDBusReply<QDBusObjectPath> reply = server.call(
        QStringLiteral("ServiceBrowserNew"), kIfaceUnspec, kProtoUnspec,
        QString::fromUtf8(kSmbType), QString(), static_cast<uint>(0));
    if (!reply.isValid()) {
        emitFinishedOnce();
        return;
    }

    m_browserPath = reply.value().path();
    if (m_browserPath.isEmpty()) {
        emitFinishedOnce();
        return;
    }

    // Hook the browser's signals. ItemNew carries only simple scalar/string
    // args, so a typed slot demarshals cleanly (int32->int, uint32->uint).
    bus.connect(QString::fromUtf8(kService), m_browserPath, QString::fromUtf8(kBrowserIface),
                QStringLiteral("ItemNew"), this,
                SLOT(onItemNew(int, int, QString, QString, QString, uint)));
    bus.connect(QString::fromUtf8(kService), m_browserPath, QString::fromUtf8(kBrowserIface),
                QStringLiteral("AllForNow"), this, SLOT(onAllForNow()));
    bus.connect(QString::fromUtf8(kService), m_browserPath, QString::fromUtf8(kBrowserIface),
                QStringLiteral("Failure"), this, SLOT(onBrowserFailure(QString)));
}

void MdnsDiscovery::stop() {
    // Free every resolver still outstanding, then the browser. Failures are
    // ignored -- the daemon may already have dropped them.
    for (const QString &path : m_resolverPaths)
        freeAvahiObject(path, kResolverIface);
    m_resolverPaths.clear();

    if (!m_browserPath.isEmpty()) {
        freeAvahiObject(m_browserPath, kBrowserIface);
        m_browserPath.clear();
    }
}

void MdnsDiscovery::onItemNew(int interface, int protocol, const QString &name,
                              const QString &type, const QString &domain, uint flags) {
    Q_UNUSED(flags);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return;

    // ServiceResolverNew(interface, protocol, name, type, domain, aprotocol,
    // flags) -> object path. aprotocol = -1 (UNSPEC): resolve to whichever
    // address family the host offers.
    QDBusInterface server(QString::fromUtf8(kService), QString::fromUtf8(kServerPath),
                          QString::fromUtf8(kServerIface), bus);
    if (!server.isValid())
        return;

    QDBusReply<QDBusObjectPath> reply = server.call(
        QStringLiteral("ServiceResolverNew"), interface, protocol, name, type, domain,
        kProtoUnspec, static_cast<uint>(0));
    if (!reply.isValid())
        return;

    const QString resolverPath = reply.value().path();
    if (resolverPath.isEmpty())
        return;
    m_resolverPaths.append(resolverPath);

    // The Found signal ends with an `aay` txt array; receiving it as a raw
    // QDBusMessage sidesteps having to demarshal that nested type.
    bus.connect(QString::fromUtf8(kService), resolverPath, QString::fromUtf8(kResolverIface),
                QStringLiteral("Found"), this, SLOT(onResolverFound(QDBusMessage)));
    bus.connect(QString::fromUtf8(kService), resolverPath, QString::fromUtf8(kResolverIface),
                QStringLiteral("Failure"), this, SLOT(onResolverFailure(QDBusMessage)));
}

void MdnsDiscovery::onAllForNow() {
    emitFinishedOnce();
}

void MdnsDiscovery::onBrowserFailure(const QString &error) {
    Q_UNUSED(error);
    emitFinishedOnce();
}

void MdnsDiscovery::onResolverFound(const QDBusMessage &message) {
    // Found(int32 interface, int32 protocol, string name, string type,
    //       string domain, string host, int32 aprotocol, string address,
    //       uint16 port, aay txt, uint32 flags)
    // Indices:  0        1        2      3       4        5      6        7
    const QList<QVariant> args = message.arguments();
    QString name;
    QString host;
    QString address;
    if (args.size() > 2)
        name = args.at(2).toString();
    if (args.size() > 5)
        host = args.at(5).toString();
    if (args.size() > 7)
        address = args.at(7).toString();

    // Prefer the resolvable host name (e.g. "nas.local"); fall back to the
    // service name when the daemon gave no host.
    const QString label = host.isEmpty() ? name : host;
    emit hostFound(label, address);

    // One-shot: this resolver has delivered, so free it now.
    const QString path = message.path();
    if (!path.isEmpty()) {
        freeAvahiObject(path, kResolverIface);
        m_resolverPaths.removeAll(path);
    }
}

void MdnsDiscovery::onResolverFailure(const QDBusMessage &message) {
    // Resolution failed for this item; drop the resolver and move on.
    const QString path = message.path();
    if (!path.isEmpty()) {
        freeAvahiObject(path, kResolverIface);
        m_resolverPaths.removeAll(path);
    }
}

void MdnsDiscovery::freeAvahiObject(const QString &path, const char *iface) {
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected() || path.isEmpty())
        return;
    QDBusInterface obj(QString::fromUtf8(kService), path, QString::fromUtf8(iface), bus);
    if (obj.isValid())
        obj.call(QStringLiteral("Free"));
}

void MdnsDiscovery::emitFinishedOnce() {
    if (m_finished)
        return;
    m_finished = true;
    emit finished();
}
