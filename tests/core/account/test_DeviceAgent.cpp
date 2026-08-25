#include <gtest/gtest.h>

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include "MockHttpServer.h"
#include "TryUntil.h"
#include "account/AccountClient.h"
#include "account/DeviceAgent.h"

// The presence socket's one timing guarantee: `announced` fires exactly when
// the account server has recorded this device's hello -- i.e. when a device
// list fetched from that moment on would show the device online, not before.
//
// This is the fix for a fresh sign-in showing itself as "offline": the first
// device list is fetched the instant the account signs in, before the agent
// socket has done its hello round trip, so without a re-fetch triggered by
// `announced` the device stays offline-looking until the user does something.
namespace {

MockHttpServer::Route json(const QByteArray &body, int status = 200) {
    MockHttpServer::Route route;
    route.status = status;
    route.body = body;
    return route;
}

QByteArray tokenReply() {
    return QJsonDocument(QJsonObject{{"access_token", QStringLiteral("access-1")},
                                     {"refresh_token", QStringLiteral("refresh-1")},
                                     {"device_id", QStringLiteral("dev-1")}})
        .toJson();
}

// Enough of /v1/agent to walk the handshake, with the one deliberate omission
// that makes it testable: receiving the hello does NOT answer "ready" -- the
// test sends that itself, so it can prove `announced` fires on "ready" and not
// a moment earlier.
class AgentPeer : public QObject {
public:
    AgentPeer() {
        m_server = new QWebSocketServer(QStringLiteral("agent"),
                                        QWebSocketServer::NonSecureMode, this);
        m_server->listen(QHostAddress::LocalHost, 0);
        connect(m_server, &QWebSocketServer::newConnection, this, [this] {
            QWebSocket *socket = m_server->nextPendingConnection();
            if (!socket)
                return;
            m_live.append(socket);
            connect(socket, &QWebSocket::disconnected, this, [this, socket] {
                m_live.removeAll(socket);
                socket->deleteLater();
            });
            connect(socket, &QWebSocket::textMessageReceived, this,
                    [this](const QString &text) {
                        if (text.contains(QStringLiteral("\"hello\"")))
                            m_gotHello = true;
                    });
            socket->sendTextMessage(QStringLiteral("{\"type\":\"welcome\"}"));
        });
    }

    // http:// on purpose: agentSocketUrl() turns it into ws://.
    QString apiUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
    }
    bool gotHello() const { return m_gotHello; }
    void sendReady() {
        for (QWebSocket *socket : m_live)
            socket->sendTextMessage(QStringLiteral("{\"type\":\"ready\"}"));
    }
    void sendPresence(const QString &deviceId, bool online) {
        const QByteArray frame = QJsonDocument(QJsonObject{{"type", QStringLiteral("presence")},
                                                           {"device_id", deviceId},
                                                           {"online", online}})
                                     .toJson(QJsonDocument::Compact);
        for (QWebSocket *socket : m_live)
            socket->sendTextMessage(QString::fromUtf8(frame));
    }
    void sendClipboardDelivery(const QString &deliveryId) {
        const QByteArray frame = QJsonDocument(QJsonObject{{"type", QStringLiteral("clipboard_delivery")},
                                                           {"delivery_id", deliveryId},
                                                           {"kind", QStringLiteral("image")},
                                                           {"size", 42}})
                                     .toJson(QJsonDocument::Compact);
        for (QWebSocket *socket : m_live)
            socket->sendTextMessage(QString::fromUtf8(frame));
    }
    void sendTicketRevoked(const QString &ticket) {
        const QByteArray frame = QJsonDocument(QJsonObject{{"type", QStringLiteral("ticket_revoked")},
                                                           {"ticket", ticket}})
                                     .toJson(QJsonDocument::Compact);
        for (QWebSocket *socket : m_live)
            socket->sendTextMessage(QString::fromUtf8(frame));
    }
    void sendTicketsRevoked(const QStringList &tickets) {
        QJsonArray values;
        for (const QString &ticket : tickets)
            values.append(ticket);
        const QByteArray frame = QJsonDocument(QJsonObject{{"type", QStringLiteral("tickets_revoked")},
                                                           {"tickets", values}})
                                     .toJson(QJsonDocument::Compact);
        for (QWebSocket *socket : m_live)
            socket->sendTextMessage(QString::fromUtf8(frame));
    }
    void sendDeviceRevoked() {
        for (QWebSocket *socket : m_live)
            socket->sendTextMessage(QStringLiteral("{\"type\":\"device_revoked\"}"));
    }

private:
    QWebSocketServer *m_server = nullptr;
    QVector<QWebSocket *> m_live;
    bool m_gotHello = false;
};

// Signs `client` in against `server`; the access token is what makes
// agentSocketUrl() non-empty, which DeviceAgent needs before it will dial.
bool signIn(AccountClient &client, MockHttpServer &server) {
    server.setRoute(QStringLiteral("/v1/auth/login"), json(tokenReply()));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    client.login(QStringLiteral("a@example.com"), QStringLiteral("correct horse"),
                 QStringLiteral("laptop"), QString());
    if (in.isEmpty())
        in.wait(5000);
    return in.count() == 1 && client.isLoggedIn();
}

} // namespace

TEST(DeviceAgent, AnnouncedFiresOnReadyNotOnConnect) {
    MockHttpServer http;
    AccountClient client;
    client.setApiUrl(http.url(QString()));
    ASSERT_TRUE(signIn(client, http));

    AgentPeer peer;
    client.setApiUrl(peer.apiUrl());

    DeviceAgent agent(&client);
    QSignalSpy announced(&agent, &DeviceAgent::announced);
    agent.start();

    // The socket has connected and sent its hello, but the server has not yet
    // said "ready" -- so "announced" must still be empty. If this emitted on
    // connect (or on sending the hello), it would already be non-zero here.
    FC_TRY_VERIFY_WITH_TIMEOUT(peer.gotHello(), 5000);
    EXPECT_TRUE(announced.isEmpty()) << "announced fired before the server's ready";

    peer.sendReady();
    FC_TRY_COMPARE_WITH_TIMEOUT(announced.count(), 1, 5000);
}

TEST(DeviceAgent, APresenceFrameEmitsPresenceChanged) {
    MockHttpServer http;
    AccountClient client;
    client.setApiUrl(http.url(QString()));
    ASSERT_TRUE(signIn(client, http));

    AgentPeer peer;
    client.setApiUrl(peer.apiUrl());

    DeviceAgent agent(&client);
    QSignalSpy presence(&agent, &DeviceAgent::presenceChanged);
    agent.start();
    FC_TRY_VERIFY_WITH_TIMEOUT(peer.gotHello(), 5000);

    peer.sendPresence(QStringLiteral("dev-2"), true);
    FC_TRY_COMPARE_WITH_TIMEOUT(presence.count(), 1, 5000);
    const QList<QVariant> online = presence.takeFirst();
    EXPECT_EQ(online.at(0).toString(), QStringLiteral("dev-2"));
    EXPECT_TRUE(online.at(1).toBool());

    peer.sendPresence(QStringLiteral("dev-2"), false);
    FC_TRY_COMPARE_WITH_TIMEOUT(presence.count(), 1, 5000);
    const QList<QVariant> offline = presence.takeFirst();
    EXPECT_EQ(offline.at(0).toString(), QStringLiteral("dev-2"));
    EXPECT_FALSE(offline.at(1).toBool());
}

TEST(DeviceAgent, ARevocationFrameEmitsTheSecuritySignals) {
    MockHttpServer http;
    AccountClient client;
    client.setApiUrl(http.url(QString()));
    ASSERT_TRUE(signIn(client, http));

    AgentPeer peer;
    client.setApiUrl(peer.apiUrl());

    DeviceAgent agent(&client);
    QSignalSpy ticket(&agent, &DeviceAgent::ticketRevoked);
    QSignalSpy device(&agent, &DeviceAgent::deviceRevoked);
    agent.start();
    FC_TRY_VERIFY_WITH_TIMEOUT(peer.gotHello(), 5000);

    peer.sendTicketRevoked(QStringLiteral("ticket-1"));
    FC_TRY_COMPARE_WITH_TIMEOUT(ticket.count(), 1, 5000);
    EXPECT_EQ(ticket.takeFirst().at(0).toString(), QStringLiteral("ticket-1"));

    peer.sendTicketsRevoked({QStringLiteral("ticket-2"), QStringLiteral("ticket-3")});
    FC_TRY_COMPARE_WITH_TIMEOUT(ticket.count(), 2, 5000);
    EXPECT_EQ(ticket.at(0).at(0).toString(), QStringLiteral("ticket-2"));
    EXPECT_EQ(ticket.at(1).at(0).toString(), QStringLiteral("ticket-3"));
    ticket.clear();

    peer.sendDeviceRevoked();
    FC_TRY_COMPARE_WITH_TIMEOUT(device.count(), 1, 5000);
}

TEST(DeviceAgent, AClipboardDeliveryFrameEmitsTheDeliveryId) {
    MockHttpServer http;
    AccountClient client;
    client.setApiUrl(http.url(QString()));
    ASSERT_TRUE(signIn(client, http));

    AgentPeer peer;
    client.setApiUrl(peer.apiUrl());

    DeviceAgent agent(&client);
    QSignalSpy available(&agent, &DeviceAgent::clipboardDeliveryAvailable);
    agent.start();
    FC_TRY_VERIFY_WITH_TIMEOUT(peer.gotHello(), 5000);

    peer.sendClipboardDelivery(QStringLiteral("delivery-1"));
    FC_TRY_COMPARE_WITH_TIMEOUT(available.count(), 1, 5000);
    EXPECT_EQ(available.takeFirst().at(0).toString(), QStringLiteral("delivery-1"));
}
