#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QVector>
#include <QWaitCondition>

#include <cstdlib>
#include <functional>
#include <thread>
#include <utility>

namespace connpool {

// Process-global bookkeeping for the detached reaper threads that
// shutdownAsync() spawns.
//
// A reaper runs protocol teardown (libssh2 -> OpenSSL, libsmbclient, ...) and
// is deliberately not joined by its spawner, so without this it can still be
// mid-disconnect when the process starts running its exit handlers. OpenSSL
// tears its own global state down from an atexit handler registered the first
// time it is used, i.e. during the initial connect -- long before any reaper
// exists. atexit runs LIFO, so an exit handler registered here (at the first
// reaper spawn) is guaranteed to run *before* OpenSSL's, which gives us a point
// to drain outstanding reapers while the crypto globals are still alive.
//
// The budget is what keeps exit responsive: a healthy disconnect takes single
// -digit milliseconds, and a stalled one is bounded by the protocol timeout the
// destroyer installs, so the wait is invisible in practice and capped in the
// worst case.
class ReaperRegistry {
public:
    // Deliberately leaked: a reaper may outlive any static destructor ordering
    // we could arrange, and destroying the registry underneath one is precisely
    // the class of bug this exists to prevent.
    static ReaperRegistry &instance() {
        static ReaperRegistry *self = new ReaperRegistry;
        return *self;
    }

    // Registers a reaper about to start. The first call installs the exit-time
    // drain (function-local static => once per process, thread-safe).
    bool enter() {
        static const bool installed = []() {
            std::atexit(&ReaperRegistry::drainAtExit);
            return true;
        }();
        (void)installed;
        QMutexLocker locker(&m_mutex);
        if (m_draining)
            return false;
        ++m_active;
        return true;
    }

    // Registers a reaper that has finished all of its teardown work.
    void leave() {
        QMutexLocker locker(&m_mutex);
        if (--m_active <= 0)
            m_cond.wakeAll();
    }

    // True once the process has begun its exit-time drain. Destroyers poll this
    // to skip graceful (network round-trip) teardown for connections they have
    // not started on yet: at this point the process is going away, so dropping
    // the socket is both sufficient and immeasurably faster.
    bool draining() const {
        QMutexLocker locker(&m_mutex);
        return m_draining;
    }

    // Waits up to timeoutMs for every registered reaper to finish. Returns false
    // if some were still running when the budget ran out.
    bool drain(int timeoutMs) {
        QElapsedTimer clock;
        clock.start();
        QMutexLocker locker(&m_mutex);
        m_draining = true;
        while (m_active > 0) {
            const qint64 left = timeoutMs - clock.elapsed();
            if (left <= 0)
                return false;
            m_cond.wait(&m_mutex, static_cast<unsigned long>(left));
        }
        return true;
    }

private:
    ReaperRegistry() = default;

    static void drainAtExit() {
        // Generous relative to the destroyers' own timeouts, so this only ever
        // bites when teardown is genuinely wedged.
        if (instance().drain(2500))
            return;
        // A reaper is still inside protocol/crypto code and would fault once the
        // remaining exit handlers free the globals under it. Nothing left to
        // flush at this point (main() has already returned), so end the process
        // here rather than let it crash on the way out.
        std::_Exit(0);
    }

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    int m_active = 0;
    bool m_draining = false;
};

// True once exit-time draining has begun; see ReaperRegistry::draining().
inline bool processIsDraining() {
    return ReaperRegistry::instance().draining();
}

} // namespace connpool

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
        // safe. It must NOT outlive the process's exit handlers, though -- the
        // registry both bounds that (drain at exit) and tells the destroyer to
        // stop being graceful once exit has begun.
        connpool::ReaperRegistry &registry = connpool::ReaperRegistry::instance();
        if (!registry.enter())
            return;
        try {
            std::thread([destroyer, toDestroy]() {
                struct ReaperLeave {
                    ~ReaperLeave() { connpool::ReaperRegistry::instance().leave(); }
                } leave;
                for (Conn *c : toDestroy) {
                    try {
                        destroyer(c);
                    } catch (...) {
                    }
                }
            }).detach();
        } catch (...) {
            registry.leave();
        }
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
