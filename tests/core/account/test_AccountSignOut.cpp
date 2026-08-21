#include <gtest/gtest.h>

#include <memory>

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

#include "CredentialStore.h"
#include "MockHttpServer.h"
#include "TryUntil.h"
#include "account/AccountClient.h"
#include "account/DeviceAgent.h"
#include "account/FileShareServer.h"
#include "config/Settings.h"

// Signing out has to leave nothing behind. Four separate claims, each of which
// is a security bug rather than a tidiness bug if it turns out to be false:
// no port still listening, no socket still announcing this device as online,
// no refresh token left in the keyring, and no token ever written to the INI.
//
// The first two used to be true only because MainWindow's loggedOut handler
// remembered to call stop() -- which no core test can reach and no future call
// site is obliged to repeat. They are now wired inside FileShareServer and
// DeviceAgent themselves, which is what these tests drive.
namespace {

MockHttpServer::Route json(const QByteArray &body, int status = 200) {
    MockHttpServer::Route route;
    route.status = status;
    route.body = body;
    return route;
}

// Distinctive on purpose: these strings are what the config.ini test greps for,
// so a token that leaked into the file could not be mistaken for anything else.
const char kAccessToken[] = "ACCESS-TOKEN-e3f1a9d4-must-never-be-written";
const char kRefreshToken[] = "REFRESH-TOKEN-7c2b8e05-must-never-be-written";
const char kPassword[] = "PASSWORD-4d9f16ca-must-never-be-written";
const char kEmail[] = "signout@example.com";

QByteArray tokenReply(const QString &deviceId) {
    return QJsonDocument(QJsonObject{{"access_token", QString::fromLatin1(kAccessToken)},
                                     {"refresh_token", QString::fromLatin1(kRefreshToken)},
                                     {"device_id", deviceId}})
        .toJson();
}

// A device id of its own per test, so a keyring entry left behind by one test
// (or by a real sign-in on this machine) can never be what another one sees.
QString freshDeviceId() {
    return QStringLiteral("dev-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void settle(int ms = 150) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Signs `client` in against `server` and returns once loggedIn() has fired.
bool signIn(AccountClient &client, MockHttpServer &server, const QString &deviceId) {
    server.setRoute(QStringLiteral("/v1/auth/login"), json(tokenReply(deviceId)));
    server.setRoute(QStringLiteral("/v1/auth/logout"), json("{}"));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    client.login(QString::fromLatin1(kEmail), QString::fromLatin1(kPassword),
                 QStringLiteral("laptop"));
    if (in.isEmpty())
        in.wait(5000);
    return in.count() == 1 && client.isLoggedIn();
}

// True when a TCP connect to this port succeeds. Used both ways round: once
// before the sign-out to prove the check can see a live port at all, and once
// after to prove the port is gone.
bool portAccepts(quint16 port) {
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, port);
    return probe.waitForConnected(2000);
}

// The keyring assertions are only meaningful where a keyring is actually
// running; same probe test_AccountClient.cpp uses.
bool keyringWorks() {
    const QString id =
        QStringLiteral("fc-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!CredentialStore::save(id, QStringLiteral("probe")).ok)
        return false;
    QString back;
    const bool ok = CredentialStore::load(id, &back).ok && back == QStringLiteral("probe");
    CredentialStore::remove(id);
    return ok;
}

// The account server's /v1/agent endpoint, enough of it for DeviceAgent to
// reach a genuinely connected state: accept, say "welcome", and count both the
// sockets still open and every dial ever made.
//
// A real socket rather than the HTTP mock 404ing the handshake, because an
// agent that never connects proves nothing -- signed out, agentSocketUrl() is
// empty and openSocket() gives up on its own, so a never-connected agent looks
// identical whether or not sign-out actually stops it.
class FakeAgentServer : public QObject {
public:
    FakeAgentServer() {
        m_server = new QWebSocketServer(QStringLiteral("agent"),
                                        QWebSocketServer::NonSecureMode, this);
        m_server->listen(QHostAddress::LocalHost, 0);
        connect(m_server, &QWebSocketServer::newConnection, this, [this] {
            QWebSocket *socket = m_server->nextPendingConnection();
            if (!socket)
                return;
            m_accepted += 1;
            m_live.append(socket);
            connect(socket, &QWebSocket::disconnected, this, [this, socket] {
                m_live.removeAll(socket);
                socket->deleteLater();
            });
            socket->sendTextMessage(QStringLiteral("{\"type\":\"welcome\"}"));
        });
    }

    // http:// on purpose: agentSocketUrl() is what turns it into ws://.
    QString apiUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
    }
    int accepted() const { return m_accepted; }
    int live() const { return m_live.size(); }

private:
    QWebSocketServer *m_server = nullptr;
    QVector<QWebSocket *> m_live;
    int m_accepted = 0;
};

// Points Settings at a directory of this test's own, so nothing here can read
// or scribble on the real config.ini. Same mechanism test_CredentialStore.cpp
// uses for ConnectionStore.
class IsolatedConfig {
public:
    IsolatedConfig() { qputenv("FILECOMMANDER_CONFIG_HOME", directory.path().toUtf8()); }
    ~IsolatedConfig() { qunsetenv("FILECOMMANDER_CONFIG_HOME"); }
    QTemporaryDir directory;
};

} // namespace

TEST(AccountSignOut, TheSharePortStopsListeningAndRefusesConnections) {
    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    const QString deviceId = freshDeviceId();
    ASSERT_TRUE(signIn(client, server, deviceId));

    FileShareServer share(&client);
    QSignalSpy up(&share, &FileShareServer::started);
    share.start();
    if (up.isEmpty())
        up.wait(5000);
    ASSERT_EQ(up.count(), 1);
    const quint16 port = share.port();
    ASSERT_NE(port, 0);
    // The other half of the check: a port that was never reachable would make
    // the assertion below pass for the wrong reason.
    ASSERT_TRUE(portAccepts(port)) << "the share port was not reachable before sign-out";

    client.logout();

    // stop() reaches the listener through the worker thread, so the port goes
    // away a moment after logout() returns rather than inside it.
    FC_TRY_VERIFY_WITH_TIMEOUT(!share.isRunning(), 5000);
    EXPECT_EQ(share.port(), 0);
    EXPECT_FALSE(portAccepts(port)) << "something is still listening on port " << port;
}

TEST(AccountSignOut, TheDeviceAgentSocketClosesAndStaysClosed) {
    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    ASSERT_TRUE(signIn(client, server, freshDeviceId()));

    // Sign-in needed the HTTP mock; the agent needs a real WebSocket peer, so
    // point the client at one now that the token is in hand. The best-effort
    // logout POST will land there instead and fail, which changes nothing --
    // logout() does not wait on it.
    FakeAgentServer agentServer;
    client.setApiUrl(agentServer.apiUrl());

    DeviceAgent agent(&client);
    QSignalSpy connected(&agent, &DeviceAgent::connected);
    agent.start();
    FC_TRY_VERIFY_WITH_TIMEOUT(agent.isConnected() && agentServer.live() == 1, 10000);
    ASSERT_EQ(connected.count(), 1);
    const int dialsAtSignOut = agentServer.accepted();

    client.logout();

    // The device stops reporting itself online: the socket the server counts as
    // this device's presence is gone from its side too, not merely idle.
    FC_TRY_VERIFY_WITH_TIMEOUT(!agent.isConnected() && agentServer.live() == 0, 5000);
    // ...and stays gone. Past the first two backoff steps (1s and 2s), so a
    // still-armed reconnect would have fired by now.
    settle(3000);
    EXPECT_EQ(agentServer.live(), 0);
    EXPECT_EQ(agentServer.accepted(), dialsAtSignOut)
        << "the agent dialled the account server again after sign-out";
    EXPECT_TRUE(client.agentSocketUrl().isEmpty());
}

TEST(AccountSignOut, TheKeyringRefreshTokenIsDeleted) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    const QString deviceId = freshDeviceId();
    ASSERT_TRUE(signIn(client, server, deviceId));

    const QString entry = QStringLiteral("account:") + deviceId;
    QString stored;
    ASSERT_TRUE(CredentialStore::load(entry, &stored).ok);
    ASSERT_EQ(stored, QString::fromLatin1(kRefreshToken));

    client.logout();

    QString leftover = QStringLiteral("unchanged");
    const PlatformResult after = CredentialStore::load(entry, &leftover);
    EXPECT_FALSE(after.ok);
    EXPECT_TRUE(leftover.isEmpty()) << "the refresh token is still in the keyring";
}

// Without a keyring the delete cannot be observed through load(), but it must
// still have been attempted -- and, more to the point, nothing may fall back to
// writing the token somewhere readable when the keyring refuses it.
TEST(AccountSignOut, WithoutAKeyringNothingIsLeftReadable) {
    IsolatedConfig isolated;
    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    const QString deviceId = freshDeviceId();
    ASSERT_TRUE(signIn(client, server, deviceId));
    client.logout();

    QString leftover = QStringLiteral("unchanged");
    CredentialStore::load(QStringLiteral("account:") + deviceId, &leftover);
    EXPECT_TRUE(leftover.isEmpty());
    EXPECT_FALSE(client.isLoggedIn());
    EXPECT_TRUE(client.account().deviceId.isEmpty());
    EXPECT_TRUE(client.account().email.isEmpty());
}

TEST(AccountSignOut, ConfigIniHoldsNoTokenAndNoPassword) {
    IsolatedConfig isolated;
    MockHttpServer server;

    // The whole cycle the dialog drives: sign in, record what AccountDialog
    // records, turn sharing on, then sign out. Heap-allocated because Settings
    // writes the INI out in its destructor, and the file has to be on disk
    // before it can be read back.
    auto settings = std::make_unique<Settings>();
    settings->setAccountServerUrl(server.url(QString()));

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    const QString deviceId = freshDeviceId();
    ASSERT_TRUE(signIn(client, server, deviceId));

    settings->setAccountEmail(client.account().email);
    settings->setAccountDeviceId(client.account().deviceId);
    settings->setDeviceSharingEnabled(true);
    settings->setSharedFolders({isolated.directory.path()});

    FileShareServer share(&client);
    QSignalSpy up(&share, &FileShareServer::started);
    share.start();
    if (up.isEmpty())
        up.wait(5000);
    ASSERT_EQ(up.count(), 1);

    client.logout();
    FC_TRY_VERIFY_WITH_TIMEOUT(!share.isRunning(), 5000);
    settings.reset();

    QFile ini(Settings::configFilePath());
    ASSERT_TRUE(ini.open(QIODevice::ReadOnly)) << Settings::configFilePath().toStdString();
    const QByteArray text = ini.readAll();

    // The file has to actually contain the bookkeeping, or the greps below
    // would be looking at an empty file and passing for free.
    ASSERT_TRUE(text.contains(kEmail)) << "config.ini never got written";
    EXPECT_TRUE(text.contains(deviceId.toUtf8()));

    EXPECT_FALSE(text.contains(kAccessToken)) << "the access token reached config.ini";
    EXPECT_FALSE(text.contains(kRefreshToken)) << "the refresh token reached config.ini";
    EXPECT_FALSE(text.contains(kPassword)) << "the password reached config.ini";
}
