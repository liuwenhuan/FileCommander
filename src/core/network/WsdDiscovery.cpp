#include "WsdDiscovery.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

namespace wsd {

namespace {

// WS-Discovery 组播端点(SOAP-over-UDP)。
const char *kMulticastAddr = "239.255.255.250";
const quint16 kMulticastPort = 3702;

// 用一个新的 MessageID 生成一份 Probe SOAP 报文。空的 <wsd:Probe/> 匹配所有设备。
QByteArray buildProbe()
{
    const QString msgId =
        QStringLiteral("urn:uuid:") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QString envelope = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<soap:Envelope "
        "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">\n"
        " <soap:Header>\n"
        "  <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>\n"
        "  <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>\n"
        "  <wsa:MessageID>%1</wsa:MessageID>\n"
        " </soap:Header>\n"
        " <soap:Body><wsd:Probe/></soap:Body>\n"
        "</soap:Envelope>\n").arg(msgId);

    return envelope.toUtf8();
}

// 从一份 ProbeMatch SOAP 里提取 <wsd:XAddrs> 内的 http URL,取出主机 IP。
// XAddrs 通常是空格分隔的一个或多个 URL,如
// "http://192.168.1.5:5357/aaaa http://[fe80::..]:5357/aaaa"。
// 只按 localName == "XAddrs" 匹配,不死磕命名空间前缀。
void extractHosts(const QByteArray &payload, QSet<QString> &hosts)
{
    QXmlStreamReader xml(payload);
    while (!xml.atEnd() && !xml.hasError()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token != QXmlStreamReader::StartElement)
            continue;
        if (xml.name().toString().compare(QStringLiteral("XAddrs"),
                                          Qt::CaseInsensitive) != 0)
            continue;

        const QString text = xml.readElementText(
            QXmlStreamReader::IncludeChildElements);
        const QStringList urls =
            text.split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        for (const QString &u : urls) {
            const QUrl url(u.trimmed());
            const QString host = url.host();
            if (!host.isEmpty())
                hosts.insert(host);
        }
    }
}

} // namespace

QVector<QPair<QString, QString>> probe(int timeoutMs)
{
    QVector<QPair<QString, QString>> result;
    if (timeoutMs <= 0)
        timeoutMs = 3000;

    QUdpSocket socket;

    // 绑定到临时端口。responders 会单播 ProbeMatch 回到这个源端口,
    // 所以要在同一 socket 上收。失败静默返回空。
    if (!socket.bind(QHostAddress(QHostAddress::AnyIPv4), 0,
                     QAbstractSocket::ShareAddress)) {
        return result;
    }

    // WSD 默认组播 TTL=1(本网段)。
    socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);

    const QByteArray probeDatagram = buildProbe();
    const QHostAddress groupAddr(QString::fromLatin1(kMulticastAddr));

    // 发送探测。写失败静默继续(仍可能收到别处已在响应的包,但一般直接返回空)。
    socket.writeDatagram(probeDatagram, groupAddr, kMulticastPort);

    // 在 timeoutMs 总时长内循环收所有 datagram。
    QSet<QString> hosts;
    QElapsedTimer timer;
    timer.start();

    while (true) {
        const qint64 elapsed = timer.elapsed();
        if (elapsed >= timeoutMs)
            break;
        const int remaining = static_cast<int>(timeoutMs - elapsed);

        // 等待有数据可读或超时。返回 false 说明超时/出错,退出循环。
        if (!socket.waitForReadyRead(remaining))
            break;

        while (socket.hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(static_cast<int>(socket.pendingDatagramSize()));
            const qint64 read = socket.readDatagram(datagram.data(),
                                                    datagram.size());
            if (read <= 0)
                continue;
            datagram.truncate(static_cast<int>(read));
            extractHosts(datagram, hosts);
        }
    }

    // 按 IP 去重,name 留空。
    result.reserve(hosts.size());
    for (const QString &host : hosts)
        result.append(qMakePair(QString(), host));

    return result;
}

} // namespace wsd
