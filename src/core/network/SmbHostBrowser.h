#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

// A discovered SMB host (a file server on the local network). `address` is
// optional and usually empty -- libsmbclient's browse list reports names, not
// addresses.
struct SmbHost {
    QString name;     // host name, e.g. "NAS01"
    QString address;  // optional; may be empty
};
// So QVector<SmbHost> can cross a queued invocation (worker sources deliver host
// batches to the browser's thread via QMetaObject::invokeMethod).
Q_DECLARE_METATYPE(SmbHost)

// Folds `found` into `known`, returning true if anything changed (so a view can
// skip a redraw when nothing did).
//
// One machine routinely arrives from several sources carrying different halves
// of its identity: mDNS and WS-Discovery report a name, often with no address,
// while the TCP-445 sweep reports an address whose reverse lookup failed. Those
// halves have to be matched up rather than letting whichever arrived first win,
// or a NAS found by name is shown without its IP for the rest of the session.
// Entries that share a full identity are collapsed; distinct machines that
// merely share a hostname (two cloned installs) stay separate because a known
// address always distinguishes them.
bool mergeDiscoveredHosts(QVector<SmbHost> &known, const QVector<SmbHost> &found);

class MdnsDiscovery;

// Discovers SMB "network neighbourhood" hosts across three complementary
// sources, because no single method covers a modern LAN:
//   * mDNS/DNS-SD (Avahi) `_smb._tcp` -- NAS, macOS, avahi-advertising Samba;
//   * WS-Discovery (multicast SOAP) -- Windows 10/11 PCs (which don't do mDNS);
//   * legacy NetBIOS browse via libsmbclient -- older networks with a browse
//     master (mostly dead on modern LANs, kept as a best-effort fallback).
// Each source runs asynchronously and reports hosts incrementally through
// hostsDiscovered; discoveryFinished is emitted exactly once, after ALL sources
// have completed (so the UI can stop showing "searching" and, if nothing was
// found, say so). Failures in any source are swallowed.
class SmbHostBrowser : public QObject {
    Q_OBJECT
public:
    explicit SmbHostBrowser(QObject *parent = nullptr);

    // Discovered hosts accumulated (deduped) from the most recent scan(s). Lets a
    // freshly opened picker show results instantly from cache instead of waiting
    // for a scan each time.
    QVector<SmbHost> cachedHosts() const { return m_cachedHosts; }

    // Starts an asynchronous discovery across all sources, UNLESS a scan is
    // already running or the cache is still fresh (< kCacheSecs old) and `force`
    // is false -- in which case the cached results are reused and no scan runs.
    // Returns true if a scan is now active (caller should show "searching" until
    // discoveryFinished), false if the fresh cache is being used (already done).
    bool startDiscovery(bool force = false);

    // Aborts the running scan: stops waiting on the in-flight sources and emits
    // discoveryFinished immediately so the UI can leave its "searching" state.
    // The concurrent workers can't be killed mid-probe, but their late results
    // are harmless (cached) and no longer re-trigger discoveryFinished. No-op if
    // no scan is active.
    void stopDiscovery();

    // How long discovered hosts stay cached before a click triggers a rescan.
    static constexpr qint64 kCacheSecs = 4 * 60 * 60; // 4 hours

signals:
    // Emitted one or more times as hosts are found; each emission carries a
    // batch (one server, or the servers of one workgroup). The receiver dedups.
    void hostsDiscovered(const QVector<SmbHost> &hosts);

    // Emitted once when EVERY source has finished (whether or not any host was
    // found).
    void discoveryFinished();

private slots:
    void onMdnsHost(const QString &name, const QString &address);
    void onSourceHosts(const QVector<SmbHost> &hosts); // a source reported a batch
    void onSourceFinished();                           // a source completed

private:
    // Dedups `hosts` against the accumulated cache (by IP, else name), appends the
    // genuinely new ones, and emits hostsDiscovered for just those.
    void addHosts(const QVector<SmbHost> &hosts);
    bool cacheFresh() const {
        return m_lastScan.isValid() && m_lastScan.secsTo(QDateTime::currentDateTime()) < kCacheSecs;
    }

    bool m_running = false;   // set/cleared on the object's thread only
    int m_pending = 0;        // sources not yet finished; discoveryFinished at 0
    MdnsDiscovery *m_mdns = nullptr;
    QSet<QString> m_localAddrs; // this machine's own IPv4s, to filter out "self"
    QVector<SmbHost> m_cachedHosts; // accumulated, deduped discovery results
    QDateTime m_lastScan;           // when the last full scan finished (cache age)
};
