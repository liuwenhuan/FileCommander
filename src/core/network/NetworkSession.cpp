#include "NetworkSession.h"

#include <QThread>
#include <QTimer>

#include "filesystem/FileProvider.h"

NetworkSession::NetworkSession(std::shared_ptr<FileProvider> provider, QObject *parent)
    : QObject(parent), m_provider(std::move(provider)) {
    // Directory listings cross from the worker thread to the GUI thread via a
    // queued signal, so their element/container types must be registered.
    qRegisterMetaType<QVector<FileInfo>>("QVector<FileInfo>");
    if (m_provider)
        m_provider->setTimeoutMs(kConnectTimeoutMs);
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("NetworkSession"));
    moveToThread(m_thread);
    // Tear the worker QObject state down on the worker thread when it finishes.
    m_thread->start();
}

NetworkSession::~NetworkSession() {
    stop();
    if (m_thread) {
        m_thread->quit();
        // Bounded by the per-op timeout: a worker mid-blocking-call returns
        // within kConnectTimeoutMs, then the loop quits.
        m_thread->wait();
        delete m_thread;
    }
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
    for (int n = 1; n <= kMaxReconnects; ++n) {
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
        if (!m_everConnected && looksLikeAuthFailure(err)) {
            setState(Idle); // clear the status line; the prompt takes over
            emit authRequired(err);
            return false;
        }
        // Backoff before the next attempt (none after the final failure):
        // 1s, 2s, 4s, 8s (capped at 8s).
        if (n < kMaxReconnects) {
            const int backoff = qMin(8000, 1000 << (n - 1));
            if (!backoffSleep(backoff))
                return false;
        }
    }
    setState(Failed);
    return false;
}

bool NetworkSession::ensureConnected() {
    if (m_state == Connected)
        return true;
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
