#include "LanScan.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QLatin1String>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

namespace {

bool isVirtualInterface(const QString &name) {
    static const char *prefixes[] = {"docker", "vethernet", "vmware", "virtualbox", "loopback", "wsl"};
    for (const char *prefix : prefixes)
        if (name.startsWith(QLatin1String(prefix), Qt::CaseInsensitive))
            return true;
    return false;
}

QString parseNbstat(const QByteArray &reply) {
    const auto *data = reinterpret_cast<const unsigned char *>(reply.constData());
    int pos = 12;
    while (pos < reply.size()) {
        const unsigned char length = data[pos];
        if (length == 0) {
            ++pos;
            break;
        }
        if ((length & 0xc0) == 0xc0) {
            pos += 2;
            break;
        }
        pos += length + 1;
    }
    if (pos + 11 > reply.size())
        return {};
    pos += 10;
    const int count = data[pos++];
    for (int i = 0; i < count && pos + 18 <= reply.size(); ++i, pos += 18) {
        const bool group = (data[pos + 16] & 0x80) != 0;
        const unsigned char suffix = data[pos + 15];
        if (group || (suffix != 0x00 && suffix != 0x20))
            continue;
        QByteArray name(reinterpret_cast<const char *>(data + pos), 15);
        while (!name.isEmpty() && (name.endsWith(' ') || name.endsWith('\0')))
            name.chop(1);
        return QString::fromLatin1(name).trimmed();
    }
    return {};
}

QString queryNetbiosName(quint32 ip) {
    QByteArray query(12, '\0');
    query[5] = 1;
    query.append(char(0x20));
    for (int i = 0; i < 16; ++i) {
        const unsigned char value = i == 0 ? '*' : 0;
        query.append(static_cast<char>('A' + ((value >> 4) & 0x0f)));
        query.append(static_cast<char>('A' + (value & 0x0f)));
    }
    query.append('\0');
    query.append("\0\x21\0\x01", 4);
    QUdpSocket socket;
    if (socket.writeDatagram(query, QHostAddress(ip), 137) < 0 || !socket.waitForReadyRead(500))
        return {};
    QByteArray reply(2048, '\0');
    const qint64 size = socket.readDatagram(reply.data(), reply.size());
    if (size <= 0)
        return {};
    reply.truncate(static_cast<int>(size));
    return parseNbstat(reply);
}

void scanBatch(const QVector<quint32> &targets, int timeoutMs, QVector<quint32> *open) {
    QEventLoop loop;
    QSet<QTcpSocket *> pending;
    auto finish = [&pending, &loop](QTcpSocket *socket) {
        if (!pending.remove(socket))
            return;
        socket->abort();
        socket->deleteLater();
        if (pending.isEmpty())
            loop.quit();
    };
    for (quint32 target : targets) {
        auto *socket = new QTcpSocket;
        pending.insert(socket);
        QObject::connect(socket, &QTcpSocket::connected, socket, [socket, target, open, &finish] {
            open->append(target);
            finish(socket);
        });
        QObject::connect(socket,
                         qOverload<QAbstractSocket::SocketError>(&QTcpSocket::errorOccurred), socket,
                         [socket, &finish](QAbstractSocket::SocketError) { finish(socket); });
        socket->connectToHost(QHostAddress(target), 445);
    }
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&pending, &loop] {
        for (QTcpSocket *socket : pending) {
            socket->abort();
            socket->deleteLater();
        }
        pending.clear();
        loop.quit();
    });
    timeout.start(qMax(100, timeoutMs));
    if (!pending.isEmpty())
        loop.exec();
}

} // namespace

QVector<QPair<QString, QString>> lanscan::scan445(int perHostTimeoutMs) {
    if (perHostTimeoutMs <= 0)
        perHostTimeoutMs = 400;
    QSet<quint32> local;
    QSet<quint32> seen;
    QVector<quint32> targets;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces)
        for (const QNetworkAddressEntry &entry : iface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                local.insert(entry.ip().toIPv4Address());

    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack) ||
            isVirtualInterface(iface.name()) || isVirtualInterface(iface.humanReadableName()))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            const quint32 ip = entry.ip().toIPv4Address();
            if ((ip & 0xffff0000u) == 0xa9fe0000u || (ip & 0xff000000u) == 0x7f000000u)
                continue;
            int prefix = entry.prefixLength();
            prefix = qBound(24, prefix, 32); // Never probe more than the local /24.
            const quint32 mask = prefix == 32 ? 0xffffffffu : 0xffffffffu << (32 - prefix);
            const quint32 first = (ip & mask) + 1;
            const quint32 last = (ip & mask) | ~mask;
            for (quint32 host = first; host < last; ++host)
                if (!local.contains(host) && !seen.contains(host)) {
                    seen.insert(host);
                    targets.append(host);
                }
        }
    }

    QVector<quint32> open;
    constexpr int kBatchSize = 96;
    for (int i = 0; i < targets.size(); i += kBatchSize) {
        QVector<quint32> batch;
        for (int j = i; j < targets.size() && j < i + kBatchSize; ++j)
            batch.append(targets.at(j));
        scanBatch(batch, perHostTimeoutMs, &open);
    }

    QVector<QPair<QString, QString>> result;
    for (quint32 ip : open)
        result.append(qMakePair(queryNetbiosName(ip), QHostAddress(ip).toString()));
    return result;
}
