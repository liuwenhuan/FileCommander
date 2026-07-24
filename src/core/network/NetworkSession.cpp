#include "NetworkSession.h"

#include <QThread>
#include <QTimer>

#include "config/Settings.h"
#include "filesystem/FileProvider.h"

NetworkSession::NetworkSession(std::shared_ptr<FileProvider> provider, QObject *parent)
    : QObject(parent), m_provider(std::move(provider)) {
    // Directory listings cross from the worker thread to the GUI thread via a
    // queued signal, so their element/container types must be registered.
    qRegisterMetaType<QVector<FileInfo>>("QVector<FileInfo>");
    if (m_provider) {
        m_provider->setTimeoutMs(kConnectTimeoutMs);
        // Size the provider's transfer connection pool to the transfer worker
        // count so it opens exactly as many independent connections as there are
        // concurrent transfers (SFTP/SMB use this; other backends ignore it).
        m_provider->setMaxTransferChannels(Settings().maxConcurrentTransfers());
    }
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("NetworkSession"));
    moveToThread(m_thread);
    // Tear the worker QObject state down on the worker thread when it finishes.
    m_thread->start();
}

NetworkSession::~NetworkSession() {
    // Reached only via the deferred-delete path in shutdownAsync (the installed
    // shared_ptr deleter), which fires after m_thread's event loop has ended --
    // so there is nothing to join here, and this runs on the worker thread, where
    // the child heartbeat QTimer is destroyed on its own affinity thread. The
    // QThread object deletes itself via its own finished->deleteLater, so we must
    // not touch m_thread here.
}

void NetworkSession::shutdownAsync() {
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_stopping = true;
    // Stop the heartbeat and let the worker's blocking call (if any) unwind on the
    // worker thread, not here.
    QMetaObject::invokeMethod(this, "onStop", Qt::QueuedConnection);
    if (!m_thread) {
        delete this; // never constructed a thread (shouldn't happen); safe direct
        return;
    }
    // Qt flushes a worker QObject's deferred delete as its thread's event loop
    // exits, so deleteLater here destroys this object on the worker thread once
    // quit() lands -- correct affinity for its child timer, and no GUI wait. The
    // thread object (GUI affinity) deletes itself the same way. Connect BEFORE
    // quit so finished() is never missed.
    connect(m_thread, &QThread::finished, this, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->quit();
    // Returns immediately; the join + deletes happen in the background.
}

void NetworkSession::start(ConnectFn connectFn, const QString &currentDir) {
    m_connectFn = std::move(connectFn);
    m_currentDir = currentDir;
    QMetaObject::invokeMethod(this, "onStart", Qt::QueuedConnection);
}

void NetworkSession::requestList(quint64 reqId, const QString &path, bool showHidden) {
    QMetaObject::invokeMethod(this, "onRequestList", Qt::QueuedConnection, Q_ARG(quint64, reqId),
                              Q_ARG(QString, path), Q_ARG(bool, showHidden));
}

void NetworkSession::retry() {
    QMetaObject::invokeMethod(this, "onRetry", Qt::QueuedConnection);
}

void NetworkSession::retryWith(ConnectFn connectFn) {
    // Safe to set here: after authRequired the session is Idle (no cycle running),
    // and the queued invoke establishes happens-before for the worker's read.
    m_connectFn = std::move(connectFn);
    QMetaObject::invokeMethod(this, "onRetryWith", Qt::QueuedConnection);
}

void NetworkSession::stop() {
    m_stopping = true;
    QMetaObject::invokeMethod(this, "onStop", Qt::QueuedConnection);
}

void NetworkSession::setState(State s, int attempt) {
    m_state = s;
    m_attempt = attempt;
    emit stateChanged(static_cast<int>(s), attempt);
}

bool NetworkSession::backoffSleep(int ms) {
    // Sleep in small slices so a stop() is honoured promptly.
    const int slice = 100;
    int slept = 0;
    while (slept < ms) {
        if (m_stopping)
            return false;
        QThread::msleep(qMin(slice, ms - slept));
        slept += slice;
    }
    return !m_stopping;
}

namespace {
// A connect failure that means "the server wants credentials" -- so retrying the
// same anonymous connect is pointless; the UI should prompt instead.
bool looksLikeAuthFailure(const QString &err) {
    const QString e = err.toLower();
    return e.contains(QStringLiteral("denied")) || e.contains(QStringLiteral("logon")) ||
           e.contains(QStringLiteral("password")) || e.contains(QStringLiteral("authentication")) ||
           e.contains(QStringLiteral("auth failed")) ||
           e.contains(QStringLiteral("nt_status_access")) ||
           e.contains(QStringLiteral("nt_status_logon")) ||
           e.contains(QStringLiteral("permission")) || e.contains(QStringLiteral("permitted"));
}
} // namespace

bool NetworkSession::reconnectCycle() {
    // A never-connected session (initial dial) gets a short attempt budget so an
    // unreachable host fails fast; a dropped-but-was-live link gets the full
    // reconnect budget. m_everConnected only flips true on success (which
    // returns), so this is stable for the whole cycle.
    const int maxAttempts = m_everConnected ? kMaxReconnects : kInitialConnectAttempts;
    QString lastErr;
    for (int n = 1; n <= maxAttempts; ++n) {
        if (m_stopping)
            return false;
        // Before the first success this is the initial connect ("connecting");
        // afterwards it is a post-drop reconnect ("reconnecting(N/M)").
        setState(m_everConnected ? Reconnecting : Connecting, n);

        QString err;
        bool ok = false;
        if (m_provider) {
            ok = m_everConnected ? m_provider->reconnect(&err)
                                 : (m_connectFn ? m_connectFn(&err) : false);
        }
        if (!ok && !err.isEmpty())
            lastErr = err; // remember the real reason to surface on final failure
        if (m_stopping)
            return false;
        if (ok) {
            m_everConnected = true;
            setState(Connected);
            if (m_heartbeat && !m_heartbeat->isActive())
                m_heartbeat->start();
            return true;
        }
        // Authentication needed on the initial connect: stop retrying anonymously
        // and ask the UI to prompt for credentials (it will call retryWith()).
        // Prefer the provider's precise auth-failure flag (real error code) and
        // fall back to keyword matching for backends that don't set it.
        const bool authNeeded =
            (m_provider && m_provider->lastConnectAuthFailed()) || looksLikeAuthFailure(err);
        if (!m_everConnected && authNeeded) {
            setState(Idle); // clear the status line; the prompt takes over
            emit authRequired(err);
            return false;
        }
        // Backoff before the next attempt (none after the final failure):
        // 1s, 2s, 4s, 8s (capped at 8s).
        if (n < maxAttempts) {
            const int backoff = qMin(8000, 1000 << (n - 1));
            if (!backoffSleep(backoff))
                return false;
        }
    }
    // Surface the real reason (connection refused / host not found / timeout /
    // auth / cert) before flipping to Failed, so the status line can show it
    // instead of a generic "reconnect failed". Emitted first so the model has it
    // stored by the time the Failed stateChanged is processed.
    emit failed(lastErr);
    setState(Failed);
    return false;
}

bool NetworkSession::ensureConnected() {
    if (m_state == Connected)
        return true;
    // Already given up (initial dial failed, or reconnect budget exhausted): don't
    // silently re-run the whole cycle on every list request -- that would keep the
    // tab stuck "connecting" and never let it settle on the failure. Rest in
    // Failed; an explicit user retry() (onRetry) restarts the cycle directly.
    if (m_state == Failed)
        return false;
    return reconnectCycle();
}

void NetworkSession::onStart() {
    if (!m_heartbeat) {
        m_heartbeat = new QTimer(this); // affinity = worker thread (we're on it)
        m_heartbeat->setInterval(kHeartbeatMs);
        connect(m_heartbeat, &QTimer::timeout, this, &NetworkSession::onHeartbeat);
    }
    m_everConnected = false;
    setState(Connecting, 0);
    reconnectCycle(); // folds the initial attempt + up to kMaxReconnects retries
}

void NetworkSession::onRequestList(quint64 reqId, const QString &path, bool showHidden) {
    if (m_stopping)
        return;
    if (!ensureConnected()) {
        emit listFailed(reqId, path);
        return;
    }

    QVector<FileInfo> entries = m_provider ? m_provider->list(path, showHidden) : QVector<FileInfo>();

    // Passive drop detection: an empty result *might* be an empty directory, or
    // it might be a dropped link. Distinguish by probing the last known-good
    // directory (which we know is a real directory): if that is now unreachable,
    // the transport dropped -> reconnect and retry the listing once.
    if (entries.isEmpty() && !m_currentDir.isEmpty() && m_provider &&
        !m_provider->isDir(m_currentDir)) {
        if (!reconnectCycle()) {
            emit listFailed(reqId, path);
            return;
        }
        entries = m_provider->list(path, showHidden);
    }

    m_currentDir = path;
    emit listReady(reqId, path, entries);
}

void NetworkSession::onRetry() {
    if (m_stopping)
        return;
    // User asked to retry after Failed: restart the cycle. Keep m_everConnected
    // as-is so the labels reflect whether we ever had a live connection.
    if (reconnectCycle() && !m_currentDir.isEmpty())
        emit listReady(0, m_currentDir, m_provider ? m_provider->list(m_currentDir, false)
                                                   : QVector<FileInfo>());
}

void NetworkSession::onRetryWith() {
    if (m_stopping)
        return;
    // Fresh connect with the new (credentialed) closure -- treat as a first
    // connect so it uses m_connectFn, not the provider's stored-credential
    // reconnect(). On success, list the directory the tab opened with.
    m_everConnected = false;
    reconnectCycle();
}

void NetworkSession::onHeartbeat() {
    if (m_stopping || m_state != Connected || m_currentDir.isEmpty() || !m_provider)
        return;
    // Liveness probe: stat the current (known-good) directory. False means the
    // transport is gone -> reconnect. A live but genuinely empty directory still
    // reports isDir==true, so this doesn't false-trigger.
    if (m_provider->isDir(m_currentDir))
        return;
    if (reconnectCycle()) {
        // Refresh the view after a transparent reconnect.
        emit listReady(0, m_currentDir, m_provider->list(m_currentDir, false));
    }
}

void NetworkSession::onStop() {
    m_stopping = true;
    if (m_heartbeat)
        m_heartbeat->stop();
}
