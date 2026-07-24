#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "filesystem/FileInfo.h"

class FileProvider;
class QThread;
class QTimer;

// Runs a network FileProvider's blocking work (connect, directory listing,
// liveness probe, reconnect) on a dedicated worker thread so the GUI thread is
// never blocked by a slow or stalled network link -- the root cause of the UI
// freezing on an unstable connection.
//
// It is an Actor: created on the GUI thread but moveToThread'd onto its own
// QThread. Every public request (start/requestList/retry/stop) is delivered to
// that thread via a queued invocation; every result (stateChanged/listReady/
// listFailed) is emitted back and, because the receivers live on the GUI thread,
// delivered there queued. The wrapped provider is only touched from the session
// thread here; the provider keeps its internal mutex so cross-provider transfer
// workers may still call it safely.
//
// It also drives the reconnect state machine: a heartbeat probe (or a failed
// listing) detects a dropped link, then it auto-reconnects with a bounded number
// of attempts and increasing backoff, surfacing "connecting / reconnecting(N/M)
// / failed" through stateChanged for the status line.
class NetworkSession : public QObject {
    Q_OBJECT

public:
    enum State { Idle, Connecting, Connected, Reconnecting, Failed };

    // The initial connection closure. Because each provider's connectToHost has
    // a different signature, the caller captures the right call; the session just
    // runs it on the worker thread. Must populate *error on failure and, on
    // success, leave the provider connected with its credentials stored (so the
    // base reconnect() can re-establish it later).
    using ConnectFn = std::function<bool(QString * /*error*/)>;

    // Tuning (milliseconds / count). Matches the agreed policy: 12s per-connect
    // timeout, up to 5 reconnect attempts, backoff 1/2/4/8/8s, 30s heartbeat.
    static constexpr int kConnectTimeoutMs = 12000;
    static constexpr int kMaxReconnects = 5;
    // A brand-new connection to an unreachable host should surface a failure
    // quickly rather than burning the full drop-recovery budget: the 5-attempt
    // backoff cycle is for a link that WAS up and blipped, not for dialling a
    // dead host. So an initial connect (never yet connected) fails after just a
    // couple of attempts.
    static constexpr int kInitialConnectAttempts = 2;
    static constexpr int kHeartbeatMs = 30000;

    explicit NetworkSession(std::shared_ptr<FileProvider> provider, QObject *parent = nullptr);
    ~NetworkSession() override;

    FileProvider *provider() const { return m_provider.get(); }
    State state() const { return m_state; }
    int attempt() const { return m_attempt; }

    // Kicks off the initial connect on the worker thread. `currentDir` is the
    // directory the tab will open, used as the heartbeat liveness probe target.
    void start(ConnectFn connectFn, const QString &currentDir);

    // Requests a directory listing on the worker thread. `reqId` lets the model
    // discard results from a superseded navigation. Auto-reconnects first if the
    // link is currently down.
    void requestList(quint64 reqId, const QString &path, bool showHidden);

    // User-initiated retry after the Failed state: restart the reconnect cycle.
    void retry();

    // Retry the initial connection with a NEW connect closure carrying credentials
    // (after authRequired). Replaces the connect closure and reruns the cycle.
    void retryWith(ConnectFn connectFn);

    // Stops the heartbeat and worker thread cleanly. Called before destruction.
    void stop();

    // Non-blocking teardown: signals the worker to stop and arranges for this
    // object and its QThread to be deleted once the thread's event loop ends,
    // WITHOUT the caller (GUI thread) waiting. A worker mid-blocking-call still
    // returns within the per-op timeout; the cleanup then completes in the
    // background instead of freezing the UI for up to 12s. This is the shared_ptr
    // deleter installed at construction, so the last owner drop tears the session
    // down asynchronously -- never call delete on a NetworkSession directly.
    void shutdownAsync();

signals:
    void stateChanged(int state, int attempt); // NetworkSession::State + reconnect attempt
    void listReady(quint64 reqId, const QString &path, const QVector<FileInfo> &entries);
    void listFailed(quint64 reqId, const QString &path);
    // The initial connection failed because the server wants credentials: the UI
    // should prompt for a username/password and call retryWith(). Emitted instead
    // of a Failed state so the anonymous attempt isn't retried 5 times in vain.
    void authRequired(const QString &error);
    // The reconnect cycle gave up. `error` is the last attempt's real reason
    // (may be empty), for a specific status-line message. Emitted just before the
    // Failed stateChanged.
    void failed(const QString &error);

private slots:
    // All of these execute on the worker thread.
    void onStart();
    void onRequestList(quint64 reqId, const QString &path, bool showHidden);
    void onRetry();
    void onRetryWith();
    void onHeartbeat();
    void onStop();

private:
    void setState(State s, int attempt = 0); // emits stateChanged
    // On the worker thread: if not currently Connected, run the reconnect cycle.
    // Returns true if the link is (now) up. Bounded by kMaxReconnects.
    bool ensureConnected();
    // The bounded reconnect loop: up to kMaxReconnects attempts with increasing
    // backoff. Before the first success it re-runs the initial connect closure
    // (state Connecting -> "connecting"); after that it uses the provider's
    // reconnect() (state Reconnecting -> "reconnecting(N/M)"). Sets Failed and
    // returns false if all attempts fail. Runs on the worker thread.
    bool reconnectCycle();
    // Interruptible backoff sleep (checks m_stopping); returns false if asked to
    // stop mid-sleep.
    bool backoffSleep(int ms);

    std::shared_ptr<FileProvider> m_provider;
    QThread *m_thread = nullptr;
    QTimer *m_heartbeat = nullptr; // created/owned on the worker thread

    ConnectFn m_connectFn;
    QString m_currentDir;          // last good directory; heartbeat probes it
    State m_state = Idle;
    int m_attempt = 0;
    bool m_everConnected = false;  // distinguishes "connecting" vs "reconnecting"
    std::atomic<bool> m_stopping{false};
    bool m_shuttingDown = false;   // guards shutdownAsync against a double call
};
