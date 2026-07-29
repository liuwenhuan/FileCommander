#include "SmbHelperClient.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>

#include "SmbHelperProtocol.h"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace smbhelper;

namespace {

constexpr const char *kHelperName = "FileCommander-smb-helper";

// How long a single request may take before the helper is declared hung. Reads
// are bounded chunks over a LAN share, so this only ever fires on a genuinely
// stuck helper -- at which point killing it and falling back beats blocking a
// thumbnail worker indefinitely.
constexpr int kRequestTimeoutMs = 20000;

// Reads exactly n bytes from fd, bounded by `deadlineMs` of total wall time.
// Returns false on timeout, EOF, or error.
bool readExact(int fd, void *buf, std::size_t n, int timeoutMs) {
    auto *p = static_cast<unsigned char *>(buf);
    std::size_t done = 0;
    while (done < n) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pr = ::poll(&pfd, 1, timeoutMs);
        if (pr == 0)
            return false; // helper went quiet
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        const ssize_t r = ::read(fd, p + done, n - done);
        if (r > 0) {
            done += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        return false; // EOF: the helper exited or crashed
    }
    return true;
}

bool writeExact(int fd, const void *buf, std::size_t n, int timeoutMs) {
    const auto *p = static_cast<const unsigned char *>(buf);
    std::size_t done = 0;
    while (done < n) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        const int pr = ::poll(&pfd, 1, timeoutMs);
        if (pr == 0)
            return false;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        const ssize_t w = ::write(fd, p + done, n - done);
        if (w > 0) {
            done += static_cast<std::size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

} // namespace

QString SmbHelperClient::helperPath() {
    // Resolved once: the answer cannot change during a run, and acquire() is on
    // the path of every thumbnail.
    static const QString path = [] {
        // Next to the running binary first, so a development build and an
        // uninstalled AppImage layout both find their own helper rather than a
        // stale system-wide one.
        const QString beside =
            QCoreApplication::applicationDirPath() + QLatin1Char('/') +
            QLatin1String(kHelperName);
        if (QFileInfo(beside).isExecutable())
            return beside;
        return QStandardPaths::findExecutable(QLatin1String(kHelperName));
    }();
    return path;
}

bool SmbHelperClient::available() { return !helperPath().isEmpty(); }

SmbHelperClient::Channel::Channel(qint64 pid, int socket, int timeoutMs)
    : m_pid(pid), m_socket(socket), m_timeoutMs(timeoutMs) {}

SmbHelperClient::Channel::~Channel() {
    // Closing the socket is the helper's shutdown signal: its blocking read
    // returns EOF and it frees its SMB context and exits on its own.
    if (m_socket >= 0)
        ::close(m_socket);
    if (m_pid > 0) {
        // Reap it, so a long session never accumulates zombies. A healthy
        // helper exits as soon as its socket closes; give it a brief grace
        // period, then insist. Either way waitpid() below collects the status,
        // so no orphan and no zombie survives this.
        for (int i = 0; i < 20; ++i) {
            const pid_t r = ::waitpid(m_pid, nullptr, WNOHANG);
            if (r == m_pid || (r < 0 && errno == ECHILD))
                return;
            ::usleep(5000); // 100ms total before escalating
        }
        ::kill(m_pid, SIGKILL);
        ::waitpid(m_pid, nullptr, 0);
    }
}

bool SmbHelperClient::Channel::writeFrame(std::uint8_t op, const QByteArray &payload) {
    if (m_socket < 0 || payload.size() > static_cast<int>(kMaxPayload))
        return false;
    // Header and payload in one buffer, so a request is never delivered as a
    // header the helper then blocks forever waiting to complete.
    QByteArray frame;
    frame.resize(static_cast<int>(kHeaderSize) + payload.size());
    writeHeader(reinterpret_cast<unsigned char *>(frame.data()),
                static_cast<std::uint32_t>(payload.size()), op);
    if (!payload.isEmpty())
        std::memcpy(frame.data() + kHeaderSize, payload.constData(),
                    static_cast<std::size_t>(payload.size()));
    return writeExact(m_socket, frame.constData(), static_cast<std::size_t>(frame.size()),
                      m_timeoutMs);
}

bool SmbHelperClient::Channel::readFrame(QByteArray *payload, bool *isError) {
    unsigned char header[kHeaderSize];
    if (!readExact(m_socket, header, sizeof header, m_timeoutMs))
        return false;
    const std::uint32_t len = getU32(header);
    if (len > kMaxPayload)
        return false; // desynchronised stream; the channel is unusable
    *isError = header[4] == static_cast<std::uint8_t>(Status::Error);
    payload->resize(static_cast<int>(len));
    if (len > 0 && !readExact(m_socket, payload->data(), len, m_timeoutMs))
        return false;
    return true;
}

bool SmbHelperClient::Channel::transact(std::uint8_t op, const QByteArray &payload,
                                        QByteArray *reply, bool *operationFailed) {
    if (m_broken)
        return false;
    if (operationFailed)
        *operationFailed = false;

    if (!writeFrame(op, payload)) {
        m_broken = true;
        return false;
    }
    bool isError = false;
    QByteArray body;
    if (!readFrame(&body, &isError)) {
        m_broken = true; // timed out, crashed, or desynchronised
        return false;
    }
    if (isError) {
        // The helper answered properly, it just could not do the thing (no such
        // file, bad handle). The connection itself is still healthy, so the
        // channel stays poolable -- only the caller's operation failed.
        if (operationFailed)
            *operationFailed = true;
        return false;
    }
    if (reply)
        *reply = body;
    return true;
}

bool SmbHelperClient::Channel::open(const QByteArray &url) {
    if (m_handle != 0)
        return false; // one file at a time per channel
    QByteArray reply;
    if (!transact(static_cast<std::uint8_t>(Op::Open), url, &reply) || reply.size() < 8)
        return false;
    m_handle = getU64(reinterpret_cast<const unsigned char *>(reply.constData()));
    return m_handle != 0;
}

qint64 SmbHelperClient::Channel::read(char *buffer, qint64 maxSize) {
    if (m_handle == 0 || !buffer || maxSize <= 0)
        return -1;
    const auto want = static_cast<std::uint32_t>(
        maxSize > static_cast<qint64>(kMaxReadChunk) ? kMaxReadChunk : maxSize);

    QByteArray request;
    request.resize(12);
    auto *p = reinterpret_cast<unsigned char *>(request.data());
    putU64(p, m_handle);
    putU32(p + 8, want);

    QByteArray reply;
    if (!transact(static_cast<std::uint8_t>(Op::Read), request, &reply))
        return -1;
    if (reply.isEmpty())
        return 0; // EOF
    std::memcpy(buffer, reply.constData(), static_cast<std::size_t>(reply.size()));
    return reply.size();
}

bool SmbHelperClient::Channel::seek(qint64 offset) {
    if (m_handle == 0 || offset < 0)
        return false;
    QByteArray request;
    request.resize(16);
    auto *p = reinterpret_cast<unsigned char *>(request.data());
    putU64(p, m_handle);
    putU64(p + 8, static_cast<std::uint64_t>(offset));
    return transact(static_cast<std::uint8_t>(Op::Seek), request, nullptr);
}

qint64 SmbHelperClient::Channel::size() {
    if (m_handle == 0)
        return -1;
    QByteArray request;
    request.resize(8);
    putU64(reinterpret_cast<unsigned char *>(request.data()), m_handle);
    QByteArray reply;
    if (!transact(static_cast<std::uint8_t>(Op::Size), request, &reply) || reply.size() < 8)
        return -1;
    return static_cast<qint64>(getU64(reinterpret_cast<const unsigned char *>(reply.constData())));
}

void SmbHelperClient::Channel::close() {
    if (m_handle == 0)
        return;
    QByteArray request;
    request.resize(8);
    putU64(reinterpret_cast<unsigned char *>(request.data()), m_handle);
    // A failure here means the helper is already gone or wedged; transact() has
    // marked the channel broken, and the pool will destroy it.
    transact(static_cast<std::uint8_t>(Op::Close), request, nullptr);
    m_handle = 0;
}

SmbHelperClient::SmbHelperClient() = default;

SmbHelperClient::~SmbHelperClient() { shutdown(); }

void SmbHelperClient::configure(const QString &host, const QString &user, const QString &password,
                                const QString &workgroup, bool anonymous, int timeoutMs) {
    QVector<Channel *> stale;
    {
        QMutexLocker locker(&m_mutex);
        m_host = host;
        m_user = user;
        m_password = password;
        m_workgroup = workgroup;
        m_anonymous = anonymous;
        m_timeoutMs = timeoutMs > 0 ? timeoutMs : m_timeoutMs;
        m_configured = !host.isEmpty();
        m_shutdown = false;
        // New credentials have proven nothing yet: the next spawn must succeed
        // before this pool may again claim parallelism.
        m_everSpawned = false;

        // Existing helpers hold the *previous* credentials, so they must go;
        // the next acquire() spawns fresh ones.
        stale = m_idle;
        m_live -= m_idle.size();
        m_idle.clear();
    }
    qDeleteAll(stale);
}

void SmbHelperClient::setMaxChannels(int channels) {
    QMutexLocker locker(&m_mutex);
    m_max = qBound(1, channels, 8);
}

SmbHelperClient::Channel *SmbHelperClient::acquire() {
    {
        QMutexLocker locker(&m_mutex);
        if (m_shutdown || !m_configured)
            return nullptr;
        if (!m_idle.isEmpty())
            return m_idle.takeLast(); // most recently used: its connection is warm
        if (m_live >= m_max)
            return nullptr; // at capacity -- caller uses the in-process path
        ++m_live;           // reserve the slot, then build off the lock
    }

    Channel *channel = spawn();
    if (!channel) {
        QMutexLocker locker(&m_mutex);
        --m_live; // hand the reserved slot back
    }
    return channel;
}

void SmbHelperClient::release(Channel *channel) {
    if (!channel)
        return;
    bool destroy = false;
    {
        QMutexLocker locker(&m_mutex);
        if (channel->broken() || m_shutdown || m_live > m_max) {
            destroy = true;
            --m_live;
        } else {
            m_idle.append(channel);
        }
    }
    if (destroy)
        delete channel; // off the lock: the destructor waits on the child
}

void SmbHelperClient::shutdown() {
    QVector<Channel *> toDestroy;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shutdown)
            return;
        m_shutdown = true;
        toDestroy = m_idle;
        m_live -= m_idle.size();
        m_idle.clear();
    }
    // Synchronous on purpose: each destructor closes a socket and reaps a child
    // that exits immediately, so this is fast, and finishing here guarantees no
    // helper outlives the application.
    qDeleteAll(toDestroy);
}

SmbHelperClient::Channel *SmbHelperClient::spawn() {
    const QString exe = helperPath();
    if (exe.isEmpty())
        return nullptr;

    QString host, user, password, workgroup;
    bool anonymous = false;
    int timeoutMs = 0;
    {
        QMutexLocker locker(&m_mutex);
        host = m_host;
        user = m_user;
        password = m_password;
        workgroup = m_workgroup;
        anonymous = m_anonymous;
        timeoutMs = m_timeoutMs;
    }
    if (host.isEmpty())
        return nullptr;

    // A socketpair rather than two pipes: one bidirectional channel means the
    // parent cannot deadlock by draining the wrong one, and the helper's stderr
    // stays inherited for diagnostics.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return nullptr;

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        return nullptr;
    }
    // The child's end becomes both its stdin and stdout; dup2 clears O_CLOEXEC
    // on the copies, so it survives the exec while the parent's end does not
    // leak into it.
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, sv[1]);

    const QByteArray exeBytes = exe.toLocal8Bit();
    char *argv[] = {const_cast<char *>(exeBytes.constData()), nullptr};
    pid_t pid = -1;
    // Credentials are NOT passed here: argv is world-readable via
    // /proc/<pid>/cmdline, so they go in the Hello frame below instead.
    const int rc = posix_spawn(&pid, exeBytes.constData(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(sv[1]); // the child owns its end now

    if (rc != 0) {
        ::close(sv[0]);
        return nullptr;
    }

    auto *channel = new Channel(pid, sv[0], timeoutMs > 0 ? timeoutMs : kRequestTimeoutMs);

    // Hello: four NUL-terminated fields then the fixed tail (see the protocol
    // header). This is where the password crosses over -- on a private
    // socketpair no other process can observe.
    QByteArray hello;
    for (const QString &field : {host, user, password, workgroup}) {
        hello += field.toUtf8();
        hello += '\0';
    }
    hello += anonymous ? '\1' : '\0';
    const int tailAt = hello.size();
    hello.resize(tailAt + 4);
    putU32(reinterpret_cast<unsigned char *>(hello.data()) + tailAt,
           static_cast<std::uint32_t>(timeoutMs > 0 ? timeoutMs : kRequestTimeoutMs));

    if (!channel->transact(static_cast<std::uint8_t>(Op::Hello), hello, nullptr)) {
        delete channel; // bad credentials or an unreachable host: no helper
        return nullptr;
    }

    // Only now is the mechanism proven end to end: spawned, connected and
    // authenticated. provenChannels() reports the real cap from here on.
    {
        QMutexLocker locker(&m_mutex);
        m_everSpawned = true;
    }
    return channel;
}

int SmbHelperClient::provenChannels() const {
    QMutexLocker locker(&m_mutex);
    return (m_everSpawned && !m_shutdown) ? m_max : 1;
}
