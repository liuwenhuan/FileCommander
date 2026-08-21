#include <gtest/gtest.h>

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimer>
#include <QUuid>

#include "CredentialStore.h"
#include "MockHttpServer.h"
#include "account/AccountClient.h"

// The account REST client. The contract worth pinning down here is the one the
// UI leans on: every request ends in exactly one signal -- including when the
// server accepts the connection and then says nothing -- and the access token
// never leaves the client except as an Authorization header.
namespace {

MockHttpServer::Route json(const QByteArray &body, int status = 200) {
    MockHttpServer::Route route;
    route.status = status;
    route.body = body;
    return route;
}

QByteArray tokenReply(const char *access, const char *refresh, const char *deviceId) {
    return QJsonDocument(QJsonObject{{"access_token", QString::fromLatin1(access)},
                                     {"refresh_token", QString::fromLatin1(refresh)},
                                     {"device_id", QString::fromLatin1(deviceId)}})
        .toJson();
}

// Runs the event loop until `spy` sees a signal or the deadline passes, then
// keeps running a little longer so a second, duplicate signal would be caught
// rather than sitting unnoticed in the queue.
void waitForSignal(QSignalSpy &spy, int timeoutMs = 5000) {
    if (spy.isEmpty())
        spy.wait(timeoutMs);
    QEventLoop settle;
    QTimer::singleShot(150, &settle, &QEventLoop::quit);
    settle.exec();
}

// Signs a client in against `server`, so the tests that need a live session do
// not each repeat the handshake.
AccountClient *signedIn(AccountClient *client, MockHttpServer &server) {
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(tokenReply("access-1", "refresh-1", "dev-1")));
    QSignalSpy in(client, &AccountClient::loggedIn);
    client->login(QStringLiteral("a@example.com"), QStringLiteral("correct horse"),
                  QStringLiteral("laptop"));
    waitForSignal(in);
    return client;
}

// The refresh path reads the token back out of the login keyring, so those
// tests are only meaningful where a keyring is actually running.
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

} // namespace

TEST(AccountClient, ASilentServerStillProducesExactlyOneFailure) {
    MockHttpServer server;
    MockHttpServer::Route silent;
    silent.silent = true; // connection accepted, no reply ever
    server.setRoute(QStringLiteral("/v1/auth/login"), silent);

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    client.setTimeoutMs(300);
    QSignalSpy failed(&client, &AccountClient::requestFailed);
    QSignalSpy in(&client, &AccountClient::loggedIn);

    client.login(QStringLiteral("a@example.com"), QStringLiteral("pw"),
                 QStringLiteral("laptop"));
    waitForSignal(failed, 3000);

    EXPECT_EQ(failed.count(), 1);
    EXPECT_EQ(in.count(), 0);
    EXPECT_FALSE(client.isLoggedIn());
}

TEST(AccountClient, SignInKeepsTheTokenOffTheAccountAndSendsItAsBearer) {
    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signedIn(&client, server);

    ASSERT_TRUE(client.isLoggedIn());
    EXPECT_EQ(client.account().email, QStringLiteral("a@example.com"));
    EXPECT_EQ(client.account().deviceId, QStringLiteral("dev-1"));

    server.setRoute(QStringLiteral("/v1/devices"),
                    json(QJsonDocument(QJsonArray{
                                           QJsonObject{{"id", "dev-1"},
                                                       {"name", "laptop"},
                                                       {"platform", "linux"},
                                                       {"self", true}},
                                           QJsonObject{{"id", "dev-2"},
                                                       {"name", "desktop"},
                                                       {"online", true},
                                                       {"lan_addrs", QJsonArray{"192.168.1.5"}},
                                                       {"shares", QJsonArray{"Downloads", "Photos"}}},
                                       })
                             .toJson()));
    QSignalSpy ready(&client, &AccountClient::devicesReady);
    client.fetchDevices();
    waitForSignal(ready);

    ASSERT_EQ(ready.count(), 1);
    const auto devices = ready.at(0).at(0).value<QVector<AccountDeviceInfo>>();
    ASSERT_EQ(devices.size(), 2);
    EXPECT_TRUE(devices.at(0).self);
    EXPECT_TRUE(devices.at(1).online);
    EXPECT_EQ(devices.at(1).lanAddresses, QStringList{QStringLiteral("192.168.1.5")});
    EXPECT_EQ(devices.at(1).shares, (QStringList{QStringLiteral("Downloads"), QStringLiteral("Photos")}));
    EXPECT_TRUE(server.lastRequestHead().contains("Authorization: Bearer access-1"));
}

TEST(AccountClient, ARejectedSignInReportsTheServersOwnReason) {
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(R"({"detail":"Incorrect email or password"})", 401));

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.login(QStringLiteral("a@example.com"), QStringLiteral("wrong"),
                 QStringLiteral("laptop"));
    waitForSignal(failed);

    ASSERT_EQ(failed.count(), 1);
    EXPECT_EQ(failed.at(0).at(0).toString(), QStringLiteral("Incorrect email or password"));
    EXPECT_FALSE(client.isLoggedIn());
}

TEST(AccountClient, AnExpiredAccessTokenIsRenewedOnceAndTheRequestReplayed) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signedIn(&client, server);
    ASSERT_TRUE(client.isLoggedIn());

    // First call 401s on the stale token, the second sees the renewed one.
    server.setRouteSequence(QStringLiteral("/v1/devices"),
                            {json(R"({"detail":"Invalid or expired token"})", 401),
                             json(QJsonDocument(QJsonArray{QJsonObject{{"id", "dev-1"}}}).toJson())});
    server.setRoute(QStringLiteral("/v1/auth/refresh"),
                    json(tokenReply("access-2", "refresh-2", "dev-1")));

    QSignalSpy ready(&client, &AccountClient::devicesReady);
    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.fetchDevices();
    waitForSignal(ready);

    EXPECT_EQ(ready.count(), 1);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/devices")), 2);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/refresh")), 1);
    EXPECT_TRUE(server.lastRequestHead().contains("Authorization: Bearer access-2"));

    client.logout();
    QString leftover;
    EXPECT_FALSE(CredentialStore::load(QStringLiteral("account:dev-1"), &leftover).ok);
}

TEST(AccountClient, AServerThatAlwaysSays401FailsOnceInsteadOfLooping) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signedIn(&client, server);
    ASSERT_TRUE(client.isLoggedIn());

    server.setRoute(QStringLiteral("/v1/devices"), json(R"({"detail":"nope"})", 401));
    server.setRoute(QStringLiteral("/v1/auth/refresh"),
                    json(tokenReply("access-2", "refresh-2", "dev-1")));

    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.fetchDevices();
    waitForSignal(failed);

    EXPECT_EQ(failed.count(), 1);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/devices")), 2); // never a third
    client.logout();
}

TEST(AccountClient, FetchingDevicesWhileSignedOutNeverReachesTheNetwork) {
    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));

    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.fetchDevices();

    ASSERT_EQ(failed.count(), 1); // synchronous: no request was ever issued
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/devices")), 0);
}
