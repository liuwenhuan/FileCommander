#include "DeviceAgent.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include "AccountClient.h"
#include "NetworkSession.h"
#include "ShareIdentity.h"

namespace {

QString encode(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

DeviceAgent::DeviceAgent(AccountClient *client, QObject *parent)
    : QObject(parent), m_client(client), m_socket(new QWebSocket(QString(),
                                                                QWebSocketProtocol::VersionLatest,
                                                                this)),
      m_heartbeat(new QTimer(this)), m_reconnect(new QTimer(this)) {
    m_heartbeat->setInterval(NetworkSession::kHeartbeatMs);
    m_reconnect->setSingleShot(true);

    connect(m_heartbeat, &QTimer::timeout, this, [this] {
        m_socket->sendTextMessage(encode({{"type", "ping"}}));
    });
    connect(m_reconnect, &QTimer::timeout, this, [this] { openSocket(); });

    connect(m_socket, &QWebSocket::connected, this, [this] {
        m_attempt = 0;
        m_heartbeat->start();
        emit connected();
        // The server sends "welcome" first; sendHello() waits for it so the
        // hello is never lost against a socket the server has not registered
        // yet.
    });
    connect(m_socket, &QWebSocket::disconnected, this, [this] {
        m_heartbeat->stop();
        emit disconnected();
        scheduleReconnect();
    });
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
            [this](QAbstractSocket::SocketError) {
                // disconnected() does not always follow a failure to connect at
                // all, so the retry is armed from here too; scheduleReconnect()
                // is idempotent because the timer is single-shot.
                m_heartbeat->stop();
                scheduleReconnect();
            });
    connect(m_socket, &QWebSocket::textMessageReceived, this, &DeviceAgent::onTextMessage);
    // Same reason FileShareServer stops itself: once signed out this device
    // must stop announcing itself as online, and openSocket()'s "keep trying"
    // would otherwise reconnect forever on a session nobody has any more.
    if (m_client)
        connect(m_client, &AccountClient::loggedOut, this, &DeviceAgent::stop);
}

DeviceAgent::~DeviceAgent() = default;

bool DeviceAgent::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void DeviceAgent::start() {
    m_wanted = true;
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        openSocket();
}

void DeviceAgent::stop() {
    m_wanted = false;
    m_attempt = 0;
    m_reconnect->stop();
    m_heartbeat->stop();
    m_socket->close();
}

void DeviceAgent::setSharePort(quint16 port) {
    if (m_sharePort == port)
        return;
    m_sharePort = port;
    if (isConnected())
        sendHello();
}

void DeviceAgent::openSocket() {
    if (!m_wanted)
        return;
    // Read the request afresh every time: it carries the access token as an
    // Authorization header, and a reconnect after a token refresh must use the
    // new one.
    const QNetworkRequest request = m_client ? m_client->agentSocketRequest() : QNetworkRequest();
    if (request.url().isEmpty()) {
        // Signed out, or signed out again between the failure and this retry.
        // Keep trying: a session restored later should bring the agent back
        // without anyone calling start() a second time.
        scheduleReconnect();
        return;
    }
    m_socket->open(request);
}

void DeviceAgent::scheduleReconnect() {
    if (!m_wanted || m_reconnect->isActive())
        return;
    static const int kBackoffMs[] = {1000, 2000, 4000, 8000};
    const int index = qMin(m_attempt, static_cast<int>(sizeof(kBackoffMs) / sizeof(int)) - 1);
    m_attempt++;
    m_reconnect->start(kBackoffMs[index]);
}

void DeviceAgent::sendHello() {
    QJsonArray addresses;
    for (const QString &address : localAddresses())
        addresses.append(address);
    // The pin travels with the address and port because it is part of "how to
    // reach this device": without it the peer has no way to tell our file
    // share from anything else that answers on that port.
    m_socket->sendTextMessage(encode({{"type", "hello"},
                                      {"lan_addrs", addresses},
                                      {"port", static_cast<int>(m_sharePort)},
                                      {"pin", ShareIdentity::local().pin}}));
}

void DeviceAgent::onTextMessage(const QString &text) {
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("welcome")) {
        sendHello();
    } else if (type == QLatin1String("ready")) {
        // The server sends this only after it has written last_seen for our
        // hello, so a device list fetched from here on shows this device online
        // -- which the caller uses to stop the fresh sign-in looking offline.
        emit announced();
    } else if (type == QLatin1String("incoming")) {
        const QString ticket = message.value(QStringLiteral("ticket")).toString();
        if (!ticket.isEmpty())
            emit ticketOffered(message.value(QStringLiteral("session_id")).toString(), ticket,
                               message.value(QStringLiteral("from")).toString(),
                               message.value(QStringLiteral("expires_in")).toInt());
    }
}

QStringList DeviceAgent::localAddresses() {
    QStringList addresses;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.isLoopback() || address.isLinkLocal())
                continue;
            // The scope id of an IPv6 address would travel badly, and the
            // server splits the stored list on commas.
            const QString text = address.toString().section(QLatin1Char('%'), 0, 0);
            if (text.isEmpty() || text.contains(QLatin1Char(',')) || addresses.contains(text))
                continue;
            addresses.append(text);
        }
    }
    return addresses;
}
