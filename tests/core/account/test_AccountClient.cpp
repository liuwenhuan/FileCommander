#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
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

bool waitForRequest(MockHttpServer &server, const QString &path, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (server.requestCount(path) == 0 && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return server.requestCount(path) > 0;
}

// Signs a client in against `server`, so the tests that need a live session do
// not each repeat the handshake.
AccountClient *signedIn(AccountClient *client, MockHttpServer &server) {
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(tokenReply("access-1", "refresh-1", "dev-1")));
    QSignalSpy in(client, &AccountClient::loggedIn);
    client->login(QStringLiteral("a@example.com"), QStringLiteral("correct horse"),
                  QStringLiteral("laptop"), QString(), true);
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

class AccountUrlEnvironmentGuard {
public:
    AccountUrlEnvironmentGuard()
        : wasSet(qEnvironmentVariableIsSet("FILECOMMANDER_ACCOUNT_API_URL")),
          previous(qgetenv("FILECOMMANDER_ACCOUNT_API_URL")) {}
    ~AccountUrlEnvironmentGuard() {
        if (wasSet)
            qputenv("FILECOMMANDER_ACCOUNT_API_URL", previous);
        else
            qunsetenv("FILECOMMANDER_ACCOUNT_API_URL");
    }

private:
    bool wasSet;
    QByteArray previous;
};

} // namespace

TEST(AccountClient, CustomInstanceUrlTakesPrecedenceOverTheOfficialEnvironmentOverride) {
    AccountUrlEnvironmentGuard environment;
    MockHttpServer official;
    MockHttpServer custom;
    qputenv("FILECOMMANDER_ACCOUNT_API_URL", official.url(QString()).toUtf8());
    official.setRoute(QStringLiteral("/v1/auth/login"),
                      json(R"({"detail":"wrong server"})", 500));
    custom.setRoute(QStringLiteral("/v1/auth/login"),
                    json(tokenReply("access", "refresh", "custom-device")));

    AccountClient client;
    client.setApiUrl(custom.url(QString()) + QLatin1Char('/'));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    client.login(QStringLiteral("a@example.com"), QStringLiteral("pw"),
                 QStringLiteral("laptop"), QString(), false);
    waitForSignal(in);

    EXPECT_EQ(in.count(), 1);
    EXPECT_EQ(custom.requestCount(QStringLiteral("/v1/auth/login")), 1);
    EXPECT_EQ(official.requestCount(QStringLiteral("/v1/auth/login")), 0);
    client.logout();
}

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
                 QStringLiteral("laptop"), QString(), true);
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
                 QStringLiteral("laptop"), QString(), true);
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

TEST(AccountClient, RememberedSignInStoresTheRefreshTokenInTheKeyring) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    AccountClient::forgetStoredSession(QStringLiteral("dev-1"));
    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signedIn(&client, server);

    QString stored;
    ASSERT_TRUE(CredentialStore::load(QStringLiteral("account:dev-1"), &stored).ok);
    EXPECT_EQ(stored, QStringLiteral("refresh-1"));
    client.logout();
}

TEST(AccountClient, UnrememberedSignInRefreshesInMemoryWithoutLeavingAKeyringToken) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    ASSERT_TRUE(CredentialStore::save(QStringLiteral("account:dev-1"),
                                      QStringLiteral("old-refresh")).ok);
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(tokenReply("access-1", "refresh-1", "dev-1")));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    client.login(QStringLiteral("a@example.com"), QStringLiteral("correct horse"),
                 QStringLiteral("laptop"), QStringLiteral("dev-1"), false);
    waitForSignal(in);
    ASSERT_EQ(in.count(), 1);

    QString stored;
    EXPECT_FALSE(CredentialStore::load(QStringLiteral("account:dev-1"), &stored).ok);

    server.setRouteSequence(QStringLiteral("/v1/devices"),
                            {json(R"({"detail":"expired"})", 401),
                             json(QJsonDocument(QJsonArray{QJsonObject{{"id", "dev-1"}}}).toJson())});
    server.setRoute(QStringLiteral("/v1/auth/refresh"),
                    json(tokenReply("access-2", "refresh-2", "dev-1")));
    QSignalSpy ready(&client, &AccountClient::devicesReady);
    client.fetchDevices();
    waitForSignal(ready);

    EXPECT_EQ(ready.count(), 1);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/auth/refresh")), 1);
    stored.clear();
    EXPECT_FALSE(CredentialStore::load(QStringLiteral("account:dev-1"), &stored).ok);
    client.logout();
}

TEST(AccountClient, SwitchingServersInvalidatesAnInFlightRestoreAndItsTokens) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    const QString oldDevice = QStringLiteral("old-server-device");
    const QString newDevice = QStringLiteral("new-server-device");
    ASSERT_TRUE(CredentialStore::save(QStringLiteral("account:") + oldDevice,
                                      QStringLiteral("old-refresh")).ok);

    MockHttpServer oldServer;
    MockHttpServer::Route delayed =
        json(tokenReply("old-access", "old-refresh-2", "old-server-device"));
    delayed.delayMs = 400;
    oldServer.setRoute(QStringLiteral("/v1/auth/refresh"), delayed);

    AccountClient client;
    client.setApiUrl(oldServer.url(QString()));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.restoreSession(QStringLiteral("old@example.com"), oldDevice);
    ASSERT_TRUE(waitForRequest(oldServer, QStringLiteral("/v1/auth/refresh")));

    MockHttpServer newServer;
    newServer.setRoute(QStringLiteral("/v1/auth/login"),
                       json(tokenReply("new-access", "new-refresh", "new-server-device")));
    newServer.setRoute(QStringLiteral("/v1/devices"), json("[]"));
    client.switchApiUrl(newServer.url(QString()));
    client.login(QStringLiteral("new@example.com"), QStringLiteral("pw"),
                 QStringLiteral("laptop"), oldDevice, true);
    waitForSignal(in);
    ASSERT_EQ(in.count(), 1);

    QEventLoop delayedReplyWindow;
    QTimer::singleShot(600, &delayedReplyWindow, &QEventLoop::quit);
    delayedReplyWindow.exec();
    EXPECT_EQ(in.count(), 1) << "the old restore must not install a second session";
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(client.account().email, QStringLiteral("new@example.com"));
    EXPECT_EQ(client.account().deviceId, newDevice);

    QSignalSpy ready(&client, &AccountClient::devicesReady);
    client.fetchDevices();
    waitForSignal(ready);
    ASSERT_EQ(ready.count(), 1);
    EXPECT_TRUE(newServer.lastRequestHead().contains("Authorization: Bearer new-access"));

    QString stored;
    EXPECT_FALSE(CredentialStore::load(QStringLiteral("account:") + oldDevice, &stored).ok);
    ASSERT_TRUE(CredentialStore::load(QStringLiteral("account:") + newDevice, &stored).ok);
    EXPECT_EQ(stored, QStringLiteral("new-refresh"));
    client.logout();
    stored.clear();
    EXPECT_FALSE(CredentialStore::load(QStringLiteral("account:") + newDevice, &stored).ok);
}

TEST(AccountClient, ManualLoginInvalidatesAnInFlightRestoreOnTheSameServer) {
    if (!keyringWorks())
        GTEST_SKIP() << "no login keyring on this machine";

    const QString deviceId = QStringLiteral("same-server-device");
    ASSERT_TRUE(CredentialStore::save(QStringLiteral("account:") + deviceId,
                                      QStringLiteral("old-refresh")).ok);
    MockHttpServer server;
    MockHttpServer::Route delayed =
        json(tokenReply("old-access", "old-refresh-2", "same-server-device"));
    delayed.delayMs = 400;
    server.setRoute(QStringLiteral("/v1/auth/refresh"), delayed);
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(tokenReply("new-access", "new-refresh", "same-server-device")));

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.restoreSession(QStringLiteral("old@example.com"), deviceId);
    ASSERT_TRUE(waitForRequest(server, QStringLiteral("/v1/auth/refresh")));

    client.login(QStringLiteral("new@example.com"), QStringLiteral("pw"),
                 QStringLiteral("laptop"), deviceId, true);
    waitForSignal(in);
    ASSERT_EQ(in.count(), 1);

    QEventLoop delayedReplyWindow;
    QTimer::singleShot(600, &delayedReplyWindow, &QEventLoop::quit);
    delayedReplyWindow.exec();
    EXPECT_EQ(in.count(), 1);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(client.account().email, QStringLiteral("new@example.com"));
    QString stored;
    ASSERT_TRUE(CredentialStore::load(QStringLiteral("account:") + deviceId, &stored).ok);
    EXPECT_EQ(stored, QStringLiteral("new-refresh"));
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

TEST(AccountClient, RequestsCarryTheProtocolVersion) {
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/auth/login"), json(tokenReply("access-1", "refresh-1", "dev-1")));

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    QSignalSpy in(&client, &AccountClient::loggedIn);
    client.login(QStringLiteral("a@example.com"), QStringLiteral("correct horse"),
                 QStringLiteral("laptop"), QString(), true);
    waitForSignal(in);

    EXPECT_TRUE(QString::fromUtf8(server.lastRequestHead())
                    .contains(QStringLiteral("X-FileCommander-Protocol: 1")))
        << "the protocol header is how the server refuses a client that is too old";
}

TEST(AccountClient, AnOldClientIsRefusedWithTheServersUpdateMessage) {
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(R"({"detail":"Your version of FileCommander is too old. Please update to continue."})",
                         426));

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    QSignalSpy update(&client, &AccountClient::updateRequired);
    QSignalSpy failed(&client, &AccountClient::requestFailed);
    client.login(QStringLiteral("a@example.com"), QStringLiteral("correct horse"),
                 QStringLiteral("laptop"), QString(), true);
    waitForSignal(update);

    ASSERT_EQ(update.count(), 1);
    EXPECT_EQ(update.takeFirst().at(0).toString(),
              QStringLiteral("Your version of FileCommander is too old. Please update to continue."));
    EXPECT_TRUE(failed.isEmpty())
        << "an update-required refusal is not a sign-in failure and must not ride requestFailed";
}
