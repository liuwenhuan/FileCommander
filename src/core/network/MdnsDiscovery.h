#pragma once

#include <QObject>
#include <QString>

#ifdef Q_OS_WIN
#include <QHash>
#include <QSet>

class QUdpSocket;
class QTimer;
#else
#include <QStringList>

class QDBusMessage;
#endif

// Discovers SMB hosts advertised on the local network over mDNS / DNS-SD
// (service type "_smb._tcp"). Linux drives Avahi through D-Bus; Windows sends
// standard multicast DNS queries through QtNetwork. Keeping the public signal
// contract identical lets SmbHostBrowser combine it with WSD and the 445 scan.
//
// Lifecycle: construct, connect to the signals, then call start(). A single
// ServiceBrowser is created; each ItemNew spawns an asynchronous
// ServiceResolver so the main thread never blocks. hostFound() fires per
// resolved service and finished() fires once the first enumeration settles
// (AllForNow) or fails. Call stop() (also called from the destructor) to
// release every browser/resolver object still held by the Avahi daemon.
class MdnsDiscovery : public QObject {
    Q_OBJECT
public:
    explicit MdnsDiscovery(QObject *parent = nullptr);
    ~MdnsDiscovery() override;

    void start(); // Begin browsing _smb._tcp via Avahi D-Bus; results via signals.
    void stop();  // Stop and free the Avahi browser/resolver objects.

signals:
    void hostFound(const QString &name, const QString &address); // name may be empty; address is an IP.
    void finished(); // First enumeration finished (AllForNow) or failed.

private slots:
#ifdef Q_OS_WIN
    void onReadyRead();
    void onTimeout();
#else
    // org.freedesktop.Avahi.ServiceBrowser signals.
    void onItemNew(int interface, int protocol, const QString &name,
                   const QString &type, const QString &domain, uint flags);
    void onAllForNow();
    void onBrowserFailure(const QString &error);

    // org.freedesktop.Avahi.ServiceResolver signals (raw message so the
    // trailing aay txt array need not be demarshalled).
    void onResolverFound(const QDBusMessage &message);
    void onResolverFailure(const QDBusMessage &message);
#endif

private:
#ifdef Q_OS_WIN
    void sendQuery(const QString &name, quint16 type);
    void publishResolvedHosts();
#else
    void freeAvahiObject(const QString &path, const char *iface);
#endif
    void emitFinishedOnce();

#ifdef Q_OS_WIN
    QUdpSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    QSet<QString> m_instances;
    QHash<QString, QString> m_serviceHosts;
    QHash<QString, QString> m_hostAddresses;
    QSet<QString> m_published;
#else
    QString m_browserPath;        // Object path of the active ServiceBrowser, if any.
    QStringList m_resolverPaths;  // Object paths of live ServiceResolvers to free.
#endif
    bool m_finished = false;      // Guards finished() against a double emit.
};
