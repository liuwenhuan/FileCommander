#include <gtest/gtest.h>

#include <QEventLoop>
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
                 QStringLiteral("laptop"));
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
