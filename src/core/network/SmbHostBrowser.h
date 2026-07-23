#pragma once

#include <QObject>
#include <QString>
#include <QVector>

// A discovered SMB host (a file server on the local network). `address` is
// optional and usually empty -- libsmbclient's browse list reports names, not
// addresses.
struct SmbHost {
    QString name;     // host name, e.g. "NAS01"
    QString address;  // optional; may be empty
};

// Discovers SMB "network neighbourhood" hosts by browsing the smb:// root and
// each workgroup with libsmbclient. The scan runs on a background thread (a
// browse can block for seconds while master browsers are queried) and reports
// hosts incrementally via hostsDiscovered as each workgroup is walked, then
// emits discoveryFinished exactly once.
//
// Failures (no browser, timeout, unreachable workgroup) are swallowed: the scan
// still ends with discoveryFinished and never crashes.
class SmbHostBrowser : public QObject {
    Q_OBJECT
public:
    explicit SmbHostBrowser(QObject *parent = nullptr);

    // Starts an asynchronous discovery. Results arrive through the signals. Safe
    // to call repeatedly: a call made while a discovery is already running is
    // ignored rather than starting a second overlapping scan.
    void startDiscovery();

signals:
    // Emitted one or more times as hosts are found; each emission carries a
    // batch (typically the servers of one workgroup).
    void hostsDiscovered(const QVector<SmbHost> &hosts);

    // Emitted once when the scan completes (whether or not any host was found).
    void discoveryFinished();

private:
    bool m_running = false;  // set/cleared on the object's thread only
};
