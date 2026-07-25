#include "SmbHostBrowser.h"

#include <QHostInfo>
#include <QNetworkInterface>
#include <QPointer>
#include <QtConcurrent>

#include "LanScan.h"
#include "MdnsDiscovery.h"
#include "WsdDiscovery.h"

namespace {

// Marshals a batch of hosts (into the dedup/cache slot) and a source-completion
// notification back to the browser's thread from a worker thread (no-op if the
// browser was destroyed).
void postHosts(QPointer<SmbHostBrowser> self, const QVector<SmbHost> &hosts) {
    if (hosts.isEmpty() || !self)
        return;
    QMetaObject::invokeMethod(self.data(), "onSourceHosts", Qt::QueuedConnection,
                              Q_ARG(QVector<SmbHost>, hosts));
}
void postSourceFinished(QPointer<SmbHostBrowser> self) {
    if (!self)
        return;
    QMetaObject::invokeMethod(self.data(), "onSourceFinished", Qt::QueuedConnection);
}
} // namespace

SmbHostBrowser::SmbHostBrowser(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QVector<SmbHost>>("QVector<SmbHost>");
}

bool SmbHostBrowser::startDiscovery(bool force) {
    if (m_running)
        return true; // a scan is already active
    if (!force && cacheFresh())
        return false; // fresh cache -- reuse it, no scan
    m_running = true;

    // Snapshot this machine's own IPv4s so "self" is filtered out of the results
    // (it advertises its own Samba via mDNS; browsing to yourself is pointless).
    m_localAddrs.clear();
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol)
            m_localAddrs.insert(a.toString());

    // Three sources run in parallel; discoveryFinished fires once all complete.
    m_pending = 3;

    QPointer<SmbHostBrowser> self(this);

    // Source 1: WS-Discovery for Windows hosts (worker thread).
    QtConcurrent::run([self]() {
        QVector<SmbHost> hosts;
        for (const auto &pair : wsd::probe(3000))
            hosts.append(SmbHost{pair.first, pair.second});
        postHosts(self, hosts);
        postSourceFinished(self);
    });

    // Source 3: active TCP-445 subnet scan -- the reliable catch-all that finds
    // plain Samba/Windows servers which advertise via none of the other methods
    // (worker thread). Names come from a NetBIOS node-status query, else the IP.
    QtConcurrent::run([self]() {
        QVector<SmbHost> hosts;
        for (const auto &pair : lanscan::scan445(400))
            hosts.append(SmbHost{pair.first, pair.second});
        postHosts(self, hosts);
        postSourceFinished(self);
    });

    // Source 4: mDNS/DNS-SD via Avahi (async D-Bus on this thread).
    if (!m_mdns) {
        m_mdns = new MdnsDiscovery(this);
        connect(m_mdns, &MdnsDiscovery::hostFound, this, &SmbHostBrowser::onMdnsHost);
        connect(m_mdns, &MdnsDiscovery::finished, this, &SmbHostBrowser::onSourceFinished);
    }
    m_mdns->start();
    return true; // a scan is now active
}

void SmbHostBrowser::stopDiscovery() {
    if (!m_running)
        return;
    m_running = false;
    m_pending = 0; // a late onSourceFinished won't re-emit discoveryFinished
    m_lastScan = QDateTime::currentDateTime(); // count it as a completed run for the TTL
    // The QtConcurrent workers keep running to completion, but postHosts() just
    // feeds the cache (harmless) and postSourceFinished() is now gated out. Tell
    // the UI the scan is over so it can drop the "searching" state.
    emit discoveryFinished();
}

void SmbHostBrowser::addHosts(const QVector<SmbHost> &hosts) {
    // Dedup against the accumulated cache by IP (else name), so a host reported by
    // several sources / on several interfaces appears once. Emit only the new ones.
    auto keyOf = [](const SmbHost &h) {
        return (h.address.isEmpty() ? h.name : h.address).toLower();
    };
    QVector<SmbHost> fresh;
    for (const SmbHost &h : hosts) {
        if (keyOf(h).isEmpty())
            continue;
        bool known = false;
        for (const SmbHost &c : m_cachedHosts)
            if (keyOf(c) == keyOf(h)) {
                known = true;
                break;
            }
        if (!known) {
            m_cachedHosts.append(h);
            fresh.append(h);
        }
    }
    if (!fresh.isEmpty())
        emit hostsDiscovered(fresh);
}

void SmbHostBrowser::onSourceHosts(const QVector<SmbHost> &hosts) {
    addHosts(hosts);
}

void SmbHostBrowser::onMdnsHost(const QString &name, const QString &address) {
    // Keep IPv4 only (an IPv6 address would list the same host twice under the
    // IP-based dedup), and drop "self" / junk (loopback, link-local, and this
    // machine's own IPs -- mDNS resolves the local host on every docker iface).
    if (address.contains(QLatin1Char(':'))) // IPv6
        return;
    if (m_localAddrs.contains(address) || address.startsWith(QStringLiteral("127.")) ||
        address.startsWith(QStringLiteral("169.254.")))
        return;
    // Self advertises _smb._tcp on docker-bridge IPs that QNetworkInterface may
    // not enumerate, so also drop it by hostname. (Applied only to mDNS: the
    // 445-scan already excludes self by IP, and a real neighbour may legitimately
    // share this host's NetBIOS name.)
    QString n = name.toLower();
    if (n.endsWith(QStringLiteral(".local")))
        n.chop(6);
    if (!n.isEmpty() && n == QHostInfo::localHostName().toLower())
        return;
    addHosts({SmbHost{name, address}});
}

void SmbHostBrowser::onSourceFinished() {
    if (m_pending > 0 && --m_pending == 0) {
        m_running = false;
        m_lastScan = QDateTime::currentDateTime(); // start the 4h cache clock
        emit discoveryFinished();
    }
}
