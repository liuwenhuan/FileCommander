#include "MdnsDiscovery.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

namespace {

constexpr quint16 kMdnsPort = 5353;
const QHostAddress kMdnsGroup(QStringLiteral("224.0.0.251"));

quint16 readU16(const QByteArray &data, int pos) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData());
    return static_cast<quint16>((bytes[pos] << 8) | bytes[pos + 1]);
}

quint32 readU32(const QByteArray &data, int pos) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData());
    return (static_cast<quint32>(bytes[pos]) << 24) | (static_cast<quint32>(bytes[pos + 1]) << 16) |
           (static_cast<quint32>(bytes[pos + 2]) << 8) | static_cast<quint32>(bytes[pos + 3]);
}

bool readDnsName(const QByteArray &data, int &pos, QString *name) {
    QStringList labels;
    QSet<int> visited;
    int cursor = pos;
    bool advanced = false;
    while (cursor < data.size()) {
        const unsigned char length = static_cast<unsigned char>(data.at(cursor));
        if (length == 0) {
            if (!advanced)
                pos = cursor + 1;
            *name = labels.join(QLatin1Char('.'));
            return true;
        }
        if ((length & 0xc0) == 0xc0) {
            if (cursor + 1 >= data.size())
                return false;
            const int target = ((length & 0x3f) << 8) |
                               static_cast<unsigned char>(data.at(cursor + 1));
            if (visited.contains(target))
                return false;
            visited.insert(target);
            if (!advanced) {
                pos = cursor + 2;
                advanced = true;
            }
            cursor = target;
            continue;
        }
        if ((length & 0xc0) != 0 || cursor + 1 + length > data.size())
            return false;
        labels.append(QString::fromUtf8(data.constData() + cursor + 1, length));
        cursor += length + 1;
    }
    return false;
}

QByteArray queryFor(const QString &name, quint16 type) {
    QByteArray query(12, '\0');
    query[5] = 1; // QDCOUNT
    for (const QString &label : name.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        const QByteArray utf8 = label.toUtf8();
        if (utf8.isEmpty() || utf8.size() > 63)
            return {};
        query.append(static_cast<char>(utf8.size()));
        query.append(utf8);
    }
    query.append('\0');
    query.append(static_cast<char>(type >> 8));
    query.append(static_cast<char>(type & 0xff));
    query.append('\0');
    query.append('\1'); // IN
    return query;
}

QString normalized(const QString &value) {
    return value.endsWith(QLatin1Char('.')) ? value.left(value.size() - 1).toLower()
                                             : value.toLower();
}

} // namespace

MdnsDiscovery::MdnsDiscovery(QObject *parent) : QObject(parent) {}

MdnsDiscovery::~MdnsDiscovery() {
    stop();
}

void MdnsDiscovery::start() {
    stop();
    m_finished = false;
    m_instances.clear();
    m_serviceHosts.clear();
    m_hostAddresses.clear();
    m_published.clear();

    m_socket = new QUdpSocket(this);
    const auto bindMode = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    if (!m_socket->bind(QHostAddress::AnyIPv4, kMdnsPort, bindMode)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        emitFinishedOnce();
        return;
    }
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if ((flags & QNetworkInterface::IsUp) && !(flags & QNetworkInterface::IsLoopBack))
            m_socket->joinMulticastGroup(kMdnsGroup, iface);
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &MdnsDiscovery::onReadyRead);

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &MdnsDiscovery::onTimeout);
    m_timeout->start(3000);
    sendQuery(QStringLiteral("_smb._tcp.local"), 12); // PTR
}

void MdnsDiscovery::stop() {
    if (m_timeout)
        m_timeout->stop();
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void MdnsDiscovery::sendQuery(const QString &name, quint16 type) {
    if (!m_socket)
        return;
    const QByteArray query = queryFor(name, type);
    if (query.isEmpty())
        return;
    bool sent = false;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack))
            continue;
        m_socket->setMulticastInterface(iface);
        sent = m_socket->writeDatagram(query, kMdnsGroup, kMdnsPort) >= 0 || sent;
    }
    if (!sent)
        m_socket->writeDatagram(query, kMdnsGroup, kMdnsPort);
}

void MdnsDiscovery::onReadyRead() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray packet;
        packet.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        if (m_socket->readDatagram(packet.data(), packet.size()) <= 0 || packet.size() < 12)
            continue;

        const int questions = readU16(packet, 4);
        const int records = readU16(packet, 6) + readU16(packet, 8) + readU16(packet, 10);
        int pos = 12;
        bool valid = true;
        for (int i = 0; i < questions && valid; ++i) {
            QString ignored;
            valid = readDnsName(packet, pos, &ignored) && pos + 4 <= packet.size();
            pos += valid ? 4 : 0;
        }
        for (int i = 0; i < records && valid; ++i) {
            QString recordName;
            valid = readDnsName(packet, pos, &recordName) && pos + 10 <= packet.size();
            if (!valid)
                break;
            const quint16 type = readU16(packet, pos);
            Q_UNUSED(readU16(packet, pos + 2));
            Q_UNUSED(readU32(packet, pos + 4));
            const int length = readU16(packet, pos + 8);
            const int dataPos = pos + 10;
            pos = dataPos + length;
            if (pos > packet.size()) {
                valid = false;
                break;
            }
            const QString key = normalized(recordName);
            if (type == 12) { // PTR
                int targetPos = dataPos;
                QString instance;
                if (readDnsName(packet, targetPos, &instance)) {
                    instance = normalized(instance);
                    if (!m_instances.contains(instance)) {
                        m_instances.insert(instance);
                        sendQuery(instance, 33); // SRV
                    }
                }
            } else if (type == 33 && length >= 7) { // SRV
                int targetPos = dataPos + 6;
                QString host;
                if (readDnsName(packet, targetPos, &host)) {
                    host = normalized(host);
                    m_serviceHosts.insert(key, host);
                    sendQuery(host, 1); // A
                }
            } else if (type == 1 && length == 4) { // A
                const auto *bytes = reinterpret_cast<const unsigned char *>(packet.constData() + dataPos);
                m_hostAddresses.insert(key, QStringLiteral("%1.%2.%3.%4")
                                                 .arg(bytes[0]).arg(bytes[1]).arg(bytes[2]).arg(bytes[3]));
            }
        }
        publishResolvedHosts();
    }
}

void MdnsDiscovery::publishResolvedHosts() {
    for (const QString &instance : m_instances) {
        const QString host = m_serviceHosts.value(instance);
        const QString address = m_hostAddresses.value(host);
        if (address.isEmpty())
            continue;
        const QString label = instance.section(QLatin1Char('.'), 0, 0);
        const QString identity = normalized(instance) + QLatin1Char('|') + address;
        if (!m_published.contains(identity)) {
            m_published.insert(identity);
            emit hostFound(label, address);
        }
    }
}

void MdnsDiscovery::onTimeout() {
    publishResolvedHosts();
    emitFinishedOnce();
}

void MdnsDiscovery::emitFinishedOnce() {
    if (m_finished)
        return;
    m_finished = true;
    emit finished();
}
