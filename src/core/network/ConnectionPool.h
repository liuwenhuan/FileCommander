#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QVector>
#include <QWaitCondition>

#include <functional>
#include <thread>
#include <utility>

// A small, generic pool of independent physical network connections, embedded
// inside a FileProvider so that concurrent transfer workers can each borrow a
// *separate* connection instead of contending on the provider's single shared
// session + mutex. It is deliberately protocol-agnostic: SFTP parameterises it
// with an SSH-session bundle, SMB with an SMBCCTX*, etc.
//
// Design:
//  - Lazy: the first borrow() builds the first connection; nothing is created
//    up front.
//  - Bounded: at most `maxSize` live connections. borrow() reuses an idle one,
//    else builds a new one while under the cap, else blocks (bounded by a
//    timeout) until another worker releases one.
//  - Lock-free hot path for the caller: the actual read()/write() happen on the
//    borrowed connection object outside this class entirely, so a bulk transfer
//    never touches the provider's interactive mutex.
//  - Non-blocking teardown: shutdownAsync() hands the idle connections to a
//    detached reaper thread so the provider destructor never blocks on a slow
//    per-connection disconnect.
//
// Conn is the (protocol-specific) connection type; the pool only ever handles
// Conn* pointers, so Conn may be an incomplete type at the point of
// instantiation as a member -- the Factory/Destroyer bodies (which need the
// complete type) live in the provider's .cpp.
template <typename Conn>
class ConnectionPool {
public:
    // Builds one authenticated, ready-to-use physical connection. Returns a
    // heap-owned Conn* on success, or nullptr with *error populated on failure.
    // Runs off any lock, on a transfer worker thread; must do its own network
    // I/O without touching the provider's interactive mutex.
    using Factory = std::function<Conn *(QString *error)>;
    // Tears down and frees a connection previously produced by the Factory. Must
    // be self-contained (must NOT capture the provider), because it may run on a
    // detached reaper thread after the provider has already been destroyed.
    using Destroyer = std::function<void(Conn *)>;

    ConnectionPool() { m_clock.start(); }
    ~ConnectionPool() { shutdownAsync(); }

    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    // Installs the connection lifecycle callbacks and the size cap. Safe to call
    // again to reconfigure (e.g. after a reconnect rebuilds credentials).
    void configure(Factory factory, Destroyer destroyer, int maxSize) {
        QMutexLocker locker(&m_mutex);
        m_factory = std::move(factory);
        m_destroyer = std::move(destroyer);
        m_max = maxSize > 0 ? maxSize : 1;
        m_shutdown = false;
        m_cond.wakeAll();
    }

    // Adjusts the maximum number of live connections at runtime.
    void setMaxSize(int maxSize) {
        QMutexLocker locker(&m_mutex);
        m_max = maxSize > 0 ? maxSize : 1;
        m_cond.wakeAll();
    }

    // Bounds how long borrow() blocks waiting for a peer to release a connection
    // once the pool is at capacity, in milliseconds.
    void setBorrowTimeoutMs(int ms) {
        QMutexLocker locker(&m_mutex);
        m_borrowTimeoutMs = ms > 0 ? ms : m_borrowTimeoutMs;
    }

    // Hands out a connection: reuse an idle one, else build a new one while under
    // the cap, else block (bounded) until one is released. Returns nullptr with
    // *error populated on timeout, build failure, or after shutdown. The caller
    // owns the connection until it release()s or discard()s it.
    Conn *borrow(QString *error) {
        Factory factory;
        {
            QMutexLocker locker(&m_mutex);
            for (;;) {
                if (m_shutdown) {
                    if (error)
                        *error = QStringLiteral("connection pool is shut down");
                    return nullptr;
                }
                if (!m_idle.isEmpty()) {
                    // Reuse the most-recently-returned connection (warm cache).
                    Conn *c = m_idle.takeLast().conn;
                    return c;
                }
                if (m_live < m_max && m_factory) {
                    // Reserve a slot, then build outside the lock (network I/O).
                    ++m_live;
                    factory = m_factory;
                    break;
                }
                // At capacity (or not yet configured): wait for a release.
                if (!m_cond.wait(&m_mutex, static_cast<unsigned long>(m_borrowTimeoutMs))) {
                    if (error)
                        *error = QStringLiteral("timed out waiting for a free connection");
                    return nullptr;
                }
            }
        }

        // Build the reserved connection with no lock held.
        QString localErr;
        Conn *c = factory(&localErr);
        if (!c) {
            QMutexLocker locker(&m_mutex);
            --m_live; // give the reserved slot back
            m_cond.wakeOne();
            if (error)
                *error = localErr.isEmpty()
                             ? QStringLiteral("failed to create a connection")
                             : localErr;
            return nullptr;
        }
        return c;
    }

    // Returns a healthy connection to the idle set for reuse. If the pool has
    // since shrunk below the live count, or has shut down, the connection is
    // destroyed instead (off the lock).
    void release(Conn *c) {
        if (!c)
            return;
        Destroyer destroyer;
        bool destroy = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_shutdown || m_live > m_max) {
                destroy = true;
                --m_live;
                destroyer = m_destroyer;
            } else {
                m_idle.append(IdleEntry{c, m_clock.elapsed()});
            }
            m_cond.wakeOne();
        }
        if (destroy && destroyer)
            destroyer(c);
    }

    // Destroys a connection that hit a physical error (never returns it to the
    // idle set), freeing its slot so a fresh one can be built.
    void discard(Conn *c) {
        Destroyer destroyer;
        {
            QMutexLocker locker(&m_mutex);
            --m_live;
            destroyer = m_destroyer;
            m_cond.wakeOne();
        }
        if (destroyer && c)
            destroyer(c);
    }

    // Destroys idle connections that have sat unused for at least idleMs. Live
    // (borrowed) connections are untouched. Runs the destroyers off the lock.
    void reapIdle(int idleMs) {
        QVector<Conn *> toDestroy;
        Destroyer destroyer;
        {
            QMutexLocker locker(&m_mutex);
            destroyer = m_destroyer;
            const qint64 now = m_clock.elapsed();
            QVector<IdleEntry> keep;
            keep.reserve(m_idle.size());
            for (const IdleEntry &e : m_idle) {
                if (now - e.idleSince >= idleMs) {
                    toDestroy.append(e.conn);
                    --m_live;
                } else {
                    keep.append(e);
                }
            }
            m_idle = keep;
        }
        if (destroyer)
            for (Conn *c : toDestroy)
                destroyer(c);
    }

    // Non-blocking teardown: marks the pool shut (so later borrow() fails and
    // later release() destroys instead of pooling), then hands the currently
    // idle connections to a detached thread that destroys them. Idempotent.
    // Connections still borrowed at this point are destroyed by their eventual
    // release()/discard(); the teardown order guarantees transfer workers have
    // already been joined, so in practice only idle connections remain.
    void shutdownAsync() {
        QVector<Conn *> toDestroy;
        Destroyer destroyer;
        {
            QMutexLocker locker(&m_mutex);
            if (m_shutdown)
                return;
            m_shutdown = true;
            destroyer = m_destroyer;
            toDestroy.reserve(m_idle.size());
            for (const IdleEntry &e : m_idle)
                toDestroy.append(e.conn);
            m_live -= m_idle.size();
            m_idle.clear();
            m_cond.wakeAll(); // release any borrower blocked at capacity
        }
        if (toDestroy.isEmpty() || !destroyer)
            return;
        // Each disconnect can block on network I/O; detach so the caller (a
        // provider destructor) is never held up. The destroyer is self-contained
        // and the connections are independent, so this outliving the provider is
        // safe.
        std::thread([destroyer, toDestroy]() {
            for (Conn *c : toDestroy)
                destroyer(c);
        }).detach();
    }

private:
    struct IdleEntry {
        Conn *conn = nullptr;
        qint64 idleSince = 0; // m_clock.elapsed() at release time
    };

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    QElapsedTimer m_clock;

    Factory m_factory;
    Destroyer m_destroyer;
    QVector<IdleEntry> m_idle;
    int m_live = 0;  // idle + borrowed + reserved-in-construction
    int m_max = 1;
    bool m_shutdown = false;
    int m_borrowTimeoutMs = 20000;
};
