#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include "account/AccountClient.h"
#include "account/DeviceAgent.h"
#include "account/FileShareServer.h"
#include "account/RelayTunnel.h"
#include "filesystem/ComputerCatalog.h"
#include "network/CurlWebDavProvider.h"

// The whole device-transfer stack against a real, deployed account server:
// register, sign two devices in, let one publish a folder, and have the other
// find it, open a session and upload a file -- once over the LAN address the
// server handed out, and once over the relay.
//
// Skipped unless FILECOMMANDER_LIVE_ACCOUNT_URL names a server, so the default
// core_tests run needs no network and no account. Everything it creates is its
// own: a fresh random account, a temporary share directory, and both keyring
// entries removed on the way out.
namespace {

QString liveUrl() {
    return qEnvironmentVariable("FILECOMMANDER_LIVE_ACCOUNT_URL");
}

QByteArray blob(int size, char seed) {
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 37 + seed) & 0xff);
    return data;
}

// Runs the event loop until `spy` has a signal or the deadline passes. Longer
// than the local-server tests allow themselves: this one crosses the internet.
bool waitFor(QSignalSpy &spy, int timeoutMs = 30000) {
    return !spy.isEmpty() || spy.wait(timeoutMs);
}

// Keeps the loop turning for `ms` without blocking it, so sockets owned by
// other threads still get serviced.
void spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

class LiveAccountServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (liveUrl().isEmpty())
            GTEST_SKIP() << "set FILECOMMANDER_LIVE_ACCOUNT_URL to run this test";

        ASSERT_TRUE(m_dir.isValid());
        m_share = m_dir.path() + QStringLiteral("/share");
        ASSERT_TRUE(QDir().mkpath(m_share));
        // Named after the real received-files folder, because "Send to Device"
        // addresses that share by the folder's own name.
        m_received = m_dir.path() + QLatin1Char('/')
                     + QFileInfo(ComputerCatalog::receivedFilesPath()).fileName();
        ASSERT_TRUE(QDir().mkpath(m_received));

        m_email = QStringLiteral("live-%1@example.invalid")
                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));
        m_password = QUuid::createUuid().toString(QUuid::WithoutBraces);

        for (AccountClient *client : {&m_a, &m_b}) {
            client->setApiUrl(liveUrl());
            client->setTimeoutMs(30000);
        }

        QSignalSpy registered(&m_a, &AccountClient::registered);
        QSignalSpy failed(&m_a, &AccountClient::requestFailed);
        m_a.registerAccount(m_email, m_password);
        ASSERT_TRUE(waitFor(registered))
            << "register produced no answer"
            << (failed.isEmpty() ? "" : failed.first().first().toString().toStdString());
        ASSERT_TRUE(failed.isEmpty()) << failed.first().first().toString().toStdString();

        m_deviceA = signIn(m_a, QStringLiteral("live-sender"));
        m_deviceB = signIn(m_b, QStringLiteral("live-receiver"));
        ASSERT_FALSE(m_deviceA.isEmpty());
        ASSERT_FALSE(m_deviceB.isEmpty());
        ASSERT_NE(m_deviceA, m_deviceB);

        startReceiver();
    }

    void TearDown() override {
        m_provider.reset();
        delete m_tunnel;
        m_tunnel = nullptr;
        delete m_serving;
        m_serving = nullptr;
        delete m_agent;
        m_agent = nullptr;
        delete m_server;
        m_server = nullptr;
        // No keyring residue: this account exists only for this run.
        for (const QString &id : {m_deviceA, m_deviceB}) {
            if (!id.isEmpty())
                AccountClient::forgetStoredSession(id);
        }
    }

    QString signIn(AccountClient &client, const QString &name) {
        QSignalSpy in(&client, &AccountClient::loggedIn);
        QSignalSpy failed(&client, &AccountClient::requestFailed);
        client.login(m_email, m_password, name);
        if (!waitFor(in)) {
            ADD_FAILURE() << "login produced no answer for " << name.toStdString()
                          << (failed.isEmpty() ? ""
                                               : failed.first().first().toString().toStdString());
            return QString();
        }
        return client.account().deviceId;
    }

    // The receiving device, wired exactly as MainWindow wires it: the share
    // server first, its port announced over the agent socket, and every ticket
    // the server pushes both accepted locally and answered on the relay.
    void startReceiver() {
        m_server = new FileShareServer(&m_b);
        m_server->setSharedFolders({m_share, m_received});
        QSignalSpy up(m_server, &FileShareServer::started);
        m_server->start();
        ASSERT_TRUE(up.wait(10000));
        m_sharePort = quint16(up.first().first().toUInt());

        m_agent = new DeviceAgent(&m_b);
        m_agent->setSharePort(m_sharePort);
        QObject::connect(m_agent, &DeviceAgent::ticketOffered, m_agent,
                         [this](const QString &sessionId, const QString &ticket, const QString &,
                                int expiresIn) {
                             m_server->addTicket(ticket, expiresIn);
                             if (!m_serving)
                                 m_serving = new RelayTunnel;
                             m_serving->serveLocal(m_b.relaySocketUrl(sessionId, ticket),
                                                   m_sharePort, 2);
                         });
        QSignalSpy connected(m_agent, &DeviceAgent::connected);
        m_agent->start();
        ASSERT_TRUE(waitFor(connected)) << "the agent socket never came up";
    }

    // The sender's view of the receiver, once the server has noticed it is
    // online. Presence travels over a socket the server has to process, so a
    // first listing may still show it offline.
    AccountDeviceInfo waitForPeerOnline() {
        for (int attempt = 0; attempt < 10; ++attempt) {
            QSignalSpy ready(&m_a, &AccountClient::devicesReady);
            m_a.fetchDevices();
            if (waitFor(ready)) {
                const auto devices = ready.first().first().value<QVector<AccountDeviceInfo>>();
                for (const AccountDeviceInfo &device : devices) {
                    if (device.id == m_deviceB && device.online)
                        return device;
                }
            }
            spin(1000);
        }
        return AccountDeviceInfo();
    }

    AccountSession openSession() {
        QSignalSpy ready(&m_a, &AccountClient::sessionReady);
        QSignalSpy failed(&m_a, &AccountClient::requestFailed);
        m_a.openSession(m_deviceB);
        if (!waitFor(ready)) {
            ADD_FAILURE() << "no session"
                          << (failed.isEmpty() ? ""
                                               : failed.first().first().toString().toStdString());
            return AccountSession();
        }
        return ready.first().first().value<AccountSession>();
    }

    // Uploads `payload` through `provider` and reads it back off the receiving
    // device's disk -- the assertion both routes have to pass.
    void expectRoundTrip(const QByteArray &payload, const QString &name) {
        expectRoundTripInto(m_share, QStringLiteral("share"), payload, name);
    }

    void expectRoundTripInto(const QString &root, const QString &share,
                             const QByteArray &payload, const QString &name) {
        const QString remote = QLatin1Char('/') + share + QLatin1Char('/') + name;
        FileHandle *out = m_provider->openWrite(remote, true);
        ASSERT_NE(out, nullptr);
        m_provider->setExpectedWriteSize(out, payload.size());
        qint64 sent = 0;
        while (sent < payload.size()) {
            const qint64 n = m_provider->write(out, payload.constData() + sent,
                                               qMin<qint64>(32 * 1024, payload.size() - sent));
            ASSERT_GT(n, 0);
            sent += n;
        }
        ASSERT_TRUE(m_provider->closeHandleStatus(out));

        QFile landed(root + QLatin1Char('/') + name);
        ASSERT_TRUE(landed.open(QIODevice::ReadOnly));
        EXPECT_EQ(landed.readAll(), payload);
    }

    // The candidate addresses in turn, retried, exactly as
    // MainWindow::deviceLink() does it: the server hands out every address the
    // peer has (a router's among them, on a machine with a second interface),
    // and it answers before the ticket it pushed has reached the peer.
    std::shared_ptr<CurlWebDavProvider> dialAny(const QStringList &hosts, int port,
                                                const QString &ticket) {
        auto provider = std::make_shared<CurlWebDavProvider>();
        provider->setTimeoutMs(30000);
        QString error;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt > 0)
                spin(500);
            for (const QString &host : hosts) {
                if (provider->connectToHost(host, port, QStringLiteral("device"), ticket,
                                            /*useHttps=*/false, &error))
                    return provider;
            }
        }
        ADD_FAILURE() << "no candidate address answered: " << error.toStdString();
        return nullptr;
    }

    std::shared_ptr<CurlWebDavProvider> dial(const QString &host, int port,
                                             const QString &ticket) {
        auto provider = std::make_shared<CurlWebDavProvider>();
        provider->setTimeoutMs(30000);
        QString error;
        if (!provider->connectToHost(host, port, QStringLiteral("device"), ticket,
                                     /*useHttps=*/false, &error)) {
            ADD_FAILURE() << "connect to " << host.toStdString() << ':' << port << ": "
                          << error.toStdString();
            return nullptr;
        }
        return provider;
    }

    QTemporaryDir m_dir;
    QString m_share;
    QString m_received;
    QString m_email;
    QString m_password;
    AccountClient m_a;
    AccountClient m_b;
    QString m_deviceA;
    QString m_deviceB;
    FileShareServer *m_server = nullptr;
    DeviceAgent *m_agent = nullptr;
    RelayTunnel *m_serving = nullptr;  // receiving side of the relay
    RelayTunnel *m_tunnel = nullptr;   // sending side
    quint16 m_sharePort = 0;
    std::shared_ptr<CurlWebDavProvider> m_provider;
};

TEST_F(LiveAccountServerTest, ThePeerShowsAsOnlineWithAddressesToDial) {
    const AccountDeviceInfo peer = waitForPeerOnline();
    ASSERT_FALSE(peer.id.isEmpty()) << "the receiving device never showed as online";
    EXPECT_FALSE(peer.self);
    EXPECT_EQ(peer.name, QStringLiteral("live-receiver"));
    // Without a port there is nothing to dial, whichever route is taken.
    const AccountSession session = openSession();
    ASSERT_FALSE(session.sessionId.isEmpty());
    EXPECT_FALSE(session.ticket.isEmpty());
    EXPECT_EQ(session.peerPort, m_sharePort);
    EXPECT_FALSE(session.peerLanAddresses.isEmpty());
}

TEST_F(LiveAccountServerTest, AFileTravelsOverTheLanAddressTheServerHandedOut) {
    ASSERT_FALSE(waitForPeerOnline().id.isEmpty());
    const AccountSession session = openSession();
    ASSERT_FALSE(session.ticket.isEmpty());
    ASSERT_FALSE(session.peerLanAddresses.isEmpty());

    // Both devices are this machine, so one of the addresses the server reports
    // is reachable; that is exactly what a same-LAN pair sees.
    m_provider = dialAny(session.peerLanAddresses, session.peerPort, session.ticket);
    ASSERT_NE(m_provider, nullptr);
    expectRoundTrip(blob(256 * 1024, 3), QStringLiteral("over-lan.bin"));
}

TEST_F(LiveAccountServerTest, AFileTravelsOverTheRelayWhenTheLanIsNotUsed) {
    ASSERT_FALSE(waitForPeerOnline().id.isEmpty());
    const AccountSession session = openSession();
    ASSERT_FALSE(session.ticket.isEmpty());

    m_tunnel = new RelayTunnel;
    const quint16 local = m_tunnel->listenLocal(m_a.relaySocketUrl(session.sessionId,
                                                                  session.ticket));
    ASSERT_NE(local, 0);
    // The receiving side parks its sockets when the ticket reaches it, which is
    // a round trip through the server; dialling before that would race it.
    spin(3000);

    m_provider = dial(QStringLiteral("127.0.0.1"), local, session.ticket);
    ASSERT_NE(m_provider, nullptr);
    expectRoundTrip(blob(256 * 1024, 9), QStringLiteral("over-relay.bin"));
}

// What "Send to Device" does: the same session and the same provider a device
// tab uses, writing into the share named after the peer's received-files
// folder -- the destination MainWindow::sendToDevice() builds.
TEST_F(LiveAccountServerTest, AFileSentToADeviceLandsInItsReceivedFilesFolder) {
    ASSERT_FALSE(waitForPeerOnline().id.isEmpty());
    const AccountSession session = openSession();
    ASSERT_FALSE(session.ticket.isEmpty());
    ASSERT_FALSE(session.peerLanAddresses.isEmpty());

    m_provider = dialAny(session.peerLanAddresses, session.peerPort, session.ticket);
    ASSERT_NE(m_provider, nullptr);

    const QString share = QFileInfo(ComputerCatalog::receivedFilesPath()).fileName();
    expectRoundTripInto(m_received, share, blob(128 * 1024, 5), QStringLiteral("sent.bin"));

    // And the folder is listed under that name, so a peer browsing the device
    // finds what was sent to it rather than having to be told where it went.
    bool listed = false;
    for (const FileInfo &info : m_provider->list(QStringLiteral("/"), /*showHidden=*/false))
        listed = listed || info.name() == share;
    EXPECT_TRUE(listed);
}

TEST_F(LiveAccountServerTest, RemovingADeviceTakesItOutOfTheAccount) {
    ASSERT_FALSE(waitForPeerOnline().id.isEmpty());

    QSignalSpy removed(&m_a, &AccountClient::deviceRemoved);
    QSignalSpy failed(&m_a, &AccountClient::requestFailed);
    m_a.removeDevice(m_deviceB);
    ASSERT_TRUE(waitFor(removed));
    ASSERT_TRUE(failed.isEmpty()) << failed.first().first().toString().toStdString();
    EXPECT_EQ(removed.first().first().toString(), m_deviceB);

    // removeDevice() refreshes the list itself, so the next devicesReady is the
    // account without that device in it.
    QSignalSpy listed(&m_a, &AccountClient::devicesReady);
    m_a.fetchDevices();
    ASSERT_TRUE(waitFor(listed));
    const auto devices = listed.last().first().value<QVector<AccountDeviceInfo>>();
    for (const AccountDeviceInfo &device : devices)
        EXPECT_NE(device.id, m_deviceB);
}

} // namespace
