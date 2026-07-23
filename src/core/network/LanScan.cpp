#include "LanScan.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLatin1String>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QSet>
#include <QUdpSocket>

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// True for interface names belonging to virtual/bridged/container devices we
// never want to sweep (docker0, br-xxxx, veth*, virbr0, vnet*, tun*, tap*).
bool isVirtualInterface(const QString &name) {
    static const char *prefixes[] = {"docker", "br-", "veth", "virbr", "vnet", "tun", "tap"};
    for (const char *p : prefixes) {
        if (name.startsWith(QLatin1String(p)))
            return true;
    }
    return false;
}

// Runs one batch of concurrent non-blocking connects to <ip>:445. Every socket
// is created non-blocking; select() waits (bounded by timeoutMs) for the fds to
// become writable, and SO_ERROR distinguishes "445 open" from refused/timed out.
// Batches stay small (<= 200) so fd numbers remain well under FD_SETSIZE.
void scanBatch(const QVector<quint32> &targets, int timeoutMs, QVector<quint32> &openOut) {
    struct Pending {
        int fd;
        quint32 ip;
    };
    QVector<Pending> pending;
    pending.reserve(targets.size());

    for (quint32 ip : targets) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
            continue;

        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(445);
        sa.sin_addr.s_addr = htonl(ip);

        const int rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
        if (rc == 0) {
            // Connected immediately (rare on LAN): port is open.
            openOut.push_back(ip);
            ::close(fd);
            continue;
        }
        if (errno != EINPROGRESS) {
            ::close(fd);
            continue;
        }
        pending.push_back({fd, ip});
    }

    QElapsedTimer timer;
    timer.start();
    while (!pending.isEmpty()) {
        const qint64 remain = static_cast<qint64>(timeoutMs) - timer.elapsed();
        if (remain <= 0)
            break;

        fd_set wset;
        FD_ZERO(&wset);
        int maxfd = -1;
        for (const Pending &p : pending) {
            if (p.fd >= 0 && p.fd < FD_SETSIZE) {
                FD_SET(p.fd, &wset);
                if (p.fd > maxfd)
                    maxfd = p.fd;
            }
        }
        if (maxfd < 0)
            break;

        struct timeval tv;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;
        const int sel = ::select(maxfd + 1, nullptr, &wset, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (sel == 0)
            break; // Overall timeout: the rest are considered closed/unreachable.

        QVector<Pending> still;
        still.reserve(pending.size());
        for (const Pending &p : pending) {
            if (p.fd < FD_SETSIZE && FD_ISSET(p.fd, &wset)) {
                int soErr = 0;
                socklen_t len = sizeof(soErr);
                if (::getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0)
                    openOut.push_back(p.ip);
                ::close(p.fd);
            } else {
                still.push_back(p);
            }
        }
        pending = still;
    }

    // Anything unresolved at the deadline: give up on it.
    for (const Pending &p : pending)
        ::close(p.fd);
}

// Parses a NetBIOS node-status (NBSTAT) response and returns the first UNIQUE
// name whose suffix byte is Workstation (0x00) or Server (0x20). Returns an
// empty string on any malformed/short packet.
QString parseNbstat(const QByteArray &r) {
    const auto *d = reinterpret_cast<const unsigned char *>(r.constData());
    const int len = r.size();
    if (len < 12)
        return QString();

    // Skip the DNS-style header (12 bytes) and the answer's encoded name. The
    // name is a sequence of labels ended by a 0 length byte or a 0xC0 pointer.
    int pos = 12;
    while (pos < len) {
        const unsigned char l = d[pos];
        if (l == 0) {
            pos += 1;
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            pos += 2; // Compression pointer terminates the name.
            break;
        }
        pos += 1 + l;
    }

    // TYPE(2) CLASS(2) TTL(4) RDLENGTH(2) precede the RDATA.
    if (pos + 10 > len)
        return QString();
    pos += 8;  // skip TYPE, CLASS, TTL
    pos += 2;  // skip RDLENGTH

    if (pos + 1 > len)
        return QString();
    const int numNames = d[pos];
    pos += 1;

    // Each entry: 15-byte name + 1 suffix byte + 2 NAME_FLAGS bytes = 18 bytes.
    for (int i = 0; i < numNames; ++i) {
        if (pos + 18 > len)
            break;
        const unsigned char suffix = d[pos + 15];
        const unsigned char flagHi = d[pos + 16];
        const bool isGroup = (flagHi & 0x80) != 0;
        if (!isGroup && (suffix == 0x00 || suffix == 0x20)) {
            QByteArray nm(reinterpret_cast<const char *>(d + pos), 15);
            while (!nm.isEmpty() && (nm.endsWith(' ') || nm.endsWith('\0')))
                nm.chop(1);
            const QString name = QString::fromLatin1(nm).trimmed();
            if (!name.isEmpty())
                return name;
        }
        pos += 18;
    }
    return QString();
}

// True for RFC1918 private IPv4 (host order): 10/8, 172.16/12, 192.168/16.
bool isPrivateV4(quint32 ip) {
    return (ip & 0xFF000000u) == 0x0A000000u ||    // 10.0.0.0/8
           (ip & 0xFFF00000u) == 0xAC100000u ||    // 172.16.0.0/12
           (ip & 0xFFFF0000u) == 0xC0A80000u;      // 192.168.0.0/16
}

// Best-effort discovery of the SECOND-hop router -- this machine's gateway's own
// upstream gateway -- via an unprivileged UDP TTL=2 probe: the hop-2 router
// answers with an ICMP Time-Exceeded whose source we read from the socket error
// queue (IP_RECVERR), no root or raw socket needed. Returns its IPv4 (host
// order) only when it is a private address (a nested-NAT inner network worth
// sweeping); returns 0 otherwise (single NAT -> hop 2 is a public ISP gateway,
// unreachable, or filtered).
quint32 upstreamPrivateGateway() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return 0;
    int ttl = 2;
    ::setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
    int on = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_RECVERR, &on, sizeof(on));

    struct sockaddr_in dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(33434); // traceroute-style unused high UDP port
    ::inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr); // any off-net target; only routing matters

    quint32 result = 0;
    const char probe = 0;
    if (::sendto(fd, &probe, sizeof(probe), 0, reinterpret_cast<struct sockaddr *>(&dst),
                 sizeof(dst)) >= 0) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLERR;
        pfd.revents = 0;
        if (::poll(&pfd, 1, 1500) > 0) {
            char cbuf[512];
            char data[128];
            struct iovec iov;
            iov.iov_base = data;
            iov.iov_len = sizeof(data);
            struct sockaddr_in from;
            std::memset(&from, 0, sizeof(from));
            struct msghdr msg;
            std::memset(&msg, 0, sizeof(msg));
            msg.msg_name = &from;
            msg.msg_namelen = sizeof(from);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            if (::recvmsg(fd, &msg, MSG_ERRQUEUE) >= 0) {
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                    if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_RECVERR) {
                        auto *ee = reinterpret_cast<struct sock_extended_err *>(CMSG_DATA(cm));
                        if (ee->ee_origin == SO_EE_ORIGIN_ICMP) {
                            auto *off = reinterpret_cast<struct sockaddr_in *>(SO_EE_OFFENDER(ee));
                            if (off->sin_family == AF_INET)
                                result = ntohl(off->sin_addr.s_addr);
                        }
                    }
                }
            }
        }
    }
    ::close(fd);
    return isPrivateV4(result) ? result : 0;
}

// Sends a NBSTAT query (UDP 137) to <ip> and returns the resolved host name, or
// an empty string if it cannot be determined within the short timeout.
QString queryNetbiosName(quint32 ip) {
    QByteArray q;
    // Header: transaction id, flags, QDCOUNT=1, others 0.
    const unsigned char header[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    q.append(reinterpret_cast<const char *>(header), 12);

    // Question name: length 0x20, first-level encoding of the 16-byte NetBIOS
    // wildcard name ("*" followed by 15 NULs), then the root label 0x00.
    q.append(char(0x20));
    unsigned char nb[16];
    nb[0] = '*';
    for (int i = 1; i < 16; ++i)
        nb[i] = 0;
    for (int i = 0; i < 16; ++i) {
        q.append(char('A' + ((nb[i] >> 4) & 0x0F)));
        q.append(char('A' + (nb[i] & 0x0F)));
    }
    q.append(char(0x00));

    // QTYPE = NBSTAT (0x0021), QCLASS = IN (0x0001).
    q.append(char(0x00));
    q.append(char(0x21));
    q.append(char(0x00));
    q.append(char(0x01));

    QUdpSocket sock;
    const QHostAddress addr(ip);
    if (sock.writeDatagram(q, addr, 137) < 0)
        return QString();
    if (!sock.waitForReadyRead(500))
        return QString();

    QByteArray resp;
    resp.resize(2048);
    const qint64 n = sock.readDatagram(resp.data(), resp.size());
    if (n <= 0)
        return QString();
    resp.resize(static_cast<int>(n));
    return parseNbstat(resp);
}

} // namespace

QVector<QPair<QString, QString>> lanscan::scan445(int perHostTimeoutMs) {
    QVector<QPair<QString, QString>> results;
    if (perHostTimeoutMs <= 0)
        perHostTimeoutMs = 400;

    const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();

    // Collect every local IPv4 address so we never scan or report ourselves.
    QSet<quint32> localIps;
    for (const QNetworkInterface &iface : ifaces) {
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            const QHostAddress ip = e.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol)
                localIps.insert(ip.toIPv4Address());
        }
    }

    // Enumerate the target /24(s) from up, non-virtual, non-loopback interfaces.
    QVector<quint32> targets;
    QSet<quint32> seen;
    for (const QNetworkInterface &iface : ifaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp))
            continue;
        if (flags & QNetworkInterface::IsLoopBack)
            continue;
        if (isVirtualInterface(iface.humanReadableName()) || isVirtualInterface(iface.name()))
            continue;

        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            const QHostAddress ip = e.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            const quint32 ipv4 = ip.toIPv4Address();
            if ((ipv4 & 0xFFFF0000u) == 0xA9FE0000u)
                continue; // link-local 169.254.0.0/16
            if ((ipv4 & 0xFF000000u) == 0x7F000000u)
                continue; // loopback 127.0.0.0/8

            int prefix = e.prefixLength();
            if (prefix < 24)
                prefix = 24; // Large subnets (e.g. /16): only sweep this /24.
            if (prefix > 32)
                prefix = 32;
            const quint32 mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
            const quint32 network = ipv4 & mask;
            const quint32 broadcast = network | (~mask);
            for (quint32 host = network + 1; host < broadcast; ++host) {
                if (localIps.contains(host))
                    continue;
                if (!seen.contains(host)) {
                    seen.insert(host);
                    targets.push_back(host);
                }
            }
        }
    }

    // Nested NAT: if this machine's gateway has a PRIVATE upstream gateway (its
    // own /24 differs from ours), sweep that /24 too, so shares one level up are
    // also found. Only one level up, and only when it is a private network.
    const quint32 upstream = upstreamPrivateGateway();
    if (upstream != 0) {
        const quint32 network = upstream & 0xFFFFFF00u; // its /24
        const quint32 broadcast = network | 0x000000FFu;
        for (quint32 host = network + 1; host < broadcast; ++host) {
            if (localIps.contains(host))
                continue;
            if (!seen.contains(host)) {
                seen.insert(host);
                targets.push_back(host);
            }
        }
    }

    if (targets.isEmpty())
        return results;

    // Concurrent 445 sweep, batched to keep fd counts modest.
    QVector<quint32> openIps;
    const int batchSize = 200;
    for (int i = 0; i < targets.size(); i += batchSize) {
        QVector<quint32> batch;
        for (int j = i; j < targets.size() && j < i + batchSize; ++j)
            batch.push_back(targets[j]);
        scanBatch(batch, perHostTimeoutMs, openIps);
    }

    // Resolve names (best effort) and assemble results.
    for (quint32 ip : openIps) {
        const QString ipStr = QHostAddress(ip).toString();
        const QString name = queryNetbiosName(ip); // May be empty.
        results.push_back(qMakePair(name, ipStr));
    }
    return results;
}
