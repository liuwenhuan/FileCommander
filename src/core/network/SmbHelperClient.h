#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QVector>

#include <sys/types.h>

#include <cstdint>

// A small pool of FileCommander-smb-helper subprocesses, each owning one independent
// libsmbclient connection, used to read several files from a share at once.
//
// Why processes rather than threads: libsmbclient is not thread-safe even
// across separate SMBCCTX contexts -- its talloc pools and parsed smb.conf are
// process-global, so two threads reading through two contexts abort inside the
// library ("Bad talloc magic value"). SmbProvider therefore caps its in-process
// pool at a single channel, which makes thumbnailing a directory of videos
// strictly serial. Giving each connection its own process gives each one its
// own copy of that global state, which is the same answer GVfs arrived at (one
// gvfsd-smb per mount).
//
// Everything here is optional acceleration. Any failure -- helper missing,
// spawn refused, protocol error, timeout -- makes acquire() hand back nothing,
// and the caller falls back to SmbProvider's ordinary in-process path. That is
// the design rule: this can be faster than the single-channel path, never
// worse.
class SmbHelperClient {
public:
    // One helper subprocess and the connection it owns. Borrowed from the pool
    // for the duration of a single file read, then returned.
    class Channel {
    public:
        ~Channel();

        Channel(const Channel &) = delete;
        Channel &operator=(const Channel &) = delete;

        // Read-only file operations, mirroring the FileProvider streaming
        // contract. `url` is a full smb:// URL, already percent-encoded by the
        // caller. Every one of them returns failure (nullopt/-1/false) if the
        // helper has died or misbehaved, after marking this channel broken so
        // the pool discards rather than reuses it.
        bool open(const QByteArray &url);
        qint64 read(char *buffer, qint64 maxSize);
        bool seek(qint64 offset);
        qint64 size();
        void close();

        // True once an I/O or protocol error was seen. A broken channel is
        // destroyed instead of returned to the pool.
        bool broken() const { return m_broken; }

    private:
        friend class SmbHelperClient;
        Channel(pid_t pid, int socket, int timeoutMs);

        // Sends one request frame and waits for its reply. Returns false on
        // timeout, a dead helper, or an Error status, having set m_broken for
        // everything except a plain operation-level Error (a missing file must
        // not poison an otherwise healthy connection).
        bool transact(std::uint8_t op, const QByteArray &payload, QByteArray *reply,
                      bool *operationFailed = nullptr);
        bool writeFrame(std::uint8_t op, const QByteArray &payload);
        bool readFrame(QByteArray *payload, bool *isError);

        // Raw POSIX process management rather than QProcess: a QProcess is tied
        // to the thread that created it and may only be driven from that
        // thread, but a pooled channel is borrowed by whichever worker thread
        // needs it next. A pid plus a socket has no thread affinity at all.
        //
        // A single socketpair also removes the classic two-pipe hazard: with
        // separate stdout and stderr the parent must drain both or a full 64 KiB
        // pipe deadlocks the helper mid-reply. The helper writes only frames, on
        // one channel, and inherits the parent's stderr for diagnostics.
        pid_t m_pid = -1;
        int m_socket = -1;
        int m_timeoutMs;
        std::uint64_t m_handle = 0; // 0 == no file open
        bool m_broken = false;
    };

    SmbHelperClient();
    ~SmbHelperClient();

    SmbHelperClient(const SmbHelperClient &) = delete;
    SmbHelperClient &operator=(const SmbHelperClient &) = delete;

    // Records the credentials every helper will be spawned with. Must be called
    // before acquire(); calling it again (after a reconnect) drops the existing
    // helpers so the next acquire() dials with the new credentials.
    void configure(const QString &host, const QString &user, const QString &password,
                   const QString &workgroup, bool anonymous, int timeoutMs);

    // Caps the number of live helper processes. Clamped to 1..8.
    void setMaxChannels(int channels);

    // Borrows a ready channel: an idle one if there is one, else a freshly
    // spawned helper while under the cap. Returns nullptr when the pool is at
    // capacity or the helper cannot be started -- the caller must then use its
    // own in-process path rather than wait.
    //
    // Deliberately non-blocking at capacity: a thumbnail is worth accelerating,
    // never worth stalling a worker for.
    Channel *acquire();

    // Returns a channel to the pool, or destroys it if it is broken or the pool
    // has shrunk/shut down.
    void release(Channel *channel);

    // Destroys every idle helper and refuses further acquire(). Borrowed
    // channels are destroyed by their eventual release(). Idempotent.
    void shutdown();

    // How many reads this pool can serve in parallel: the channel cap once a
    // helper has actually been spawned and handshaken, otherwise 1.
    //
    // Deliberately reports the proven number, not the configured one. A caller
    // sizing a worker pool must not be promised parallelism that a missing or
    // broken helper cannot deliver -- in that case the reads fall back to the
    // caller's serial in-process path, where extra workers only queue.
    int provenChannels() const;

    // Whether a helper executable was found at all. False means this whole
    // mechanism is unavailable and every acquire() will return nullptr.
    static bool available();

    // Absolute path to the helper executable, or an empty string if it is not
    // installed. Resolved once and cached.
    static QString helperPath();

private:
    // Spawns one helper and completes its Hello handshake. Returns nullptr on
    // any failure. Runs without m_mutex held: a dial can take seconds and must
    // not block other workers from borrowing an already-idle channel.
    Channel *spawn();

    mutable QMutex m_mutex;
    QVector<Channel *> m_idle;
    int m_live = 0; // idle + borrowed + reserved-in-construction
    int m_max = 4;
    bool m_shutdown = false;
    bool m_configured = false;
    bool m_everSpawned = false; // a helper completed its handshake at least once

    // Credentials for spawning helpers. Guarded by m_mutex and copied out
    // before each spawn, since spawn() runs off the lock.
    QString m_host;
    QString m_user;
    QString m_password;
    QString m_workgroup;
    bool m_anonymous = false;
    int m_timeoutMs = 12000;
};
