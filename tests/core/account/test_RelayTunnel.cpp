#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>

#include "account/FileShareServer.h"
#include "account/RelayTunnel.h"
#include "account/ShareIdentity.h"
#include "network/CurlWebDavProvider.h"

// The relay half of device transfer, end to end and in one process: a stand-in
// for the account server's /v1/relay, both halves of RelayTunnel, and a real
// FileShareServer behind it. The assertion that matters is the same one the
// design rests on -- CurlWebDavProvider, unchanged, works over the tunnel.
namespace {

const char kTicket[] = "relay-ticket";

QByteArray blob(int size, char seed) {
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 31 + seed) & 0xff);
    return data;
}

// A minimal stand-in for the server's relay: pairs one "accept" socket with one
// "connect" socket and copies binary frames between them. Frames that arrive
// before a socket is paired are held, because the accessing side starts talking
// as soon as curl does, which may be before its peer has parked.
class FakeRelay : public QObject {
    Q_OBJECT

public slots:
    int start() {
        m_server = new QWebSocketServer(QStringLiteral("relay"),
                                        QWebSocketServer::NonSecureMode, this);
        if (!m_server->listen(QHostAddress::LocalHost, 0))
            return 0;
        connect(m_server, &QWebSocketServer::newConnection, this, &FakeRelay::onConnection);
        return m_server->serverPort();
    }

    int parked() const { return m_parked.size(); }

private:
    void onConnection() {
        QWebSocket *socket = m_server->nextPendingConnection();
        socket->setParent(this);
        connect(socket, &QWebSocket::binaryMessageReceived, this,
                [this, socket](const QByteArray &bytes) {
                    if (QWebSocket *peer = m_peers.value(socket))
                        peer->sendBinaryMessage(bytes);
                    else
                        m_pending[socket].append(bytes);
                });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            if (QWebSocket *peer = m_peers.take(socket)) {
                m_peers.remove(peer);
                peer->close();
            }
            m_parked.removeAll(socket);
            m_waiting.removeAll(socket);
            m_pending.remove(socket);
            socket->deleteLater();
        });

        if (socket->requestUrl().query().contains(QLatin1String("role=accept")))
            m_parked.append(socket);
        else
            m_waiting.append(socket);
        pair();
    }

    void pair() {
        while (!m_parked.isEmpty() && !m_waiting.isEmpty()) {
            QWebSocket *a = m_parked.takeFirst();
            QWebSocket *b = m_waiting.takeFirst();
            m_peers.insert(a, b);
            m_peers.insert(b, a);
            // Told before anything is forwarded: the accepting side only opens
            // its local connection once it hears this.
            a->sendTextMessage(QStringLiteral("{\"type\":\"paired\"}"));
            b->sendTextMessage(QStringLiteral("{\"type\":\"paired\"}"));
            for (QWebSocket *side : {a, b}) {
                const QByteArray held = m_pending.take(side);
                if (!held.isEmpty())
                    m_peers.value(side)->sendBinaryMessage(held);
            }
        }
    }

    QWebSocketServer *m_server = nullptr;
    QVector<QWebSocket *> m_parked;
    QVector<QWebSocket *> m_waiting;
    QHash<QWebSocket *, QWebSocket *> m_peers;
    QHash<QWebSocket *, QByteArray> m_pending;
};

// Everything runs on its own thread for the same reason the production code
// does: the test drives the provider synchronously, so a relay sharing this
// thread would never get to forward anything.
class RelayTunnelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_share = m_dir.path() + QStringLiteral("/share");
        ASSERT_TRUE(QDir().mkpath(m_share));
        QFile hello(m_share + QStringLiteral("/hello.txt"));
        ASSERT_TRUE(hello.open(QIODevice::WriteOnly));
        ASSERT_EQ(hello.write("hello over the relay"), 20);
        hello.close();

        m_relayThread.start();
        m_relay = new FakeRelay;
        m_relay->moveToThread(&m_relayThread);
        int relayPort = 0;
        QMetaObject::invokeMethod(m_relay, "start", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(int, relayPort));
        ASSERT_NE(relayPort, 0);
        m_relayUrl = QStringLiteral("ws://127.0.0.1:%1/v1/relay/session?ticket=%2")
                         .arg(relayPort)
                         .arg(QString::fromLatin1(kTicket));

        m_server = new FileShareServer;
        m_server->setSharedFolders({m_share});
        m_server->addTicket(QString::fromLatin1(kTicket), 300);
        QSignalSpy up(m_server, &FileShareServer::started);
        m_server->start();
        ASSERT_TRUE(up.wait(5000));
        const quint16 sharePort = quint16(up.first().first().toUInt());

        m_serving = new RelayTunnel;
        m_serving->serveLocal(m_relayUrl, sharePort, 2);
        // The accessing side must find a socket already parked; otherwise the
        // first request races the pool coming up.
        ASSERT_TRUE(waitForParked(2));

        m_accessing = new RelayTunnel;
        m_localPort = m_accessing->listenLocal(m_relayUrl);
        ASSERT_NE(m_localPort, 0);
    }

    void TearDown() override {
        m_provider.reset();
        delete m_accessing;
        delete m_serving;
        delete m_server;
        m_relayThread.quit();
        m_relayThread.wait();
        delete m_relay;
    }

    bool waitForParked(int count) {
        for (int i = 0; i < 100; ++i) {
            int now = 0;
            QMetaObject::invokeMethod(m_relay, "parked", Qt::BlockingQueuedConnection,
                                      Q_RETURN_ARG(int, now));
            if (now >= count)
                return true;
            QThread::msleep(50);
        }
        return false;
    }

    bool connectProvider() {
        m_provider = std::make_shared<CurlWebDavProvider>();
        m_provider->setTimeoutMs(15000);
        // The relay is a raw byte pipe, so the TLS session runs end to end
        // between this provider and the share server on the far side -- which
        // is the point: whoever runs the relay sees ciphertext.
        m_provider->setPinnedPublicKey(ShareIdentity::local().pin);
        return m_provider->connectToHost(QStringLiteral("127.0.0.1"), int(m_localPort),
                                         QStringLiteral("device"),
                                         QString::fromLatin1(kTicket),
                                         /*useHttps=*/true, &m_error);
    }

    QTemporaryDir m_dir;
    QString m_share;
    QThread m_relayThread;
    FakeRelay *m_relay = nullptr;
    QString m_relayUrl;
    FileShareServer *m_server = nullptr;
    RelayTunnel *m_serving = nullptr;
    RelayTunnel *m_accessing = nullptr;
    quint16 m_localPort = 0;
    QString m_error;
    std::shared_ptr<CurlWebDavProvider> m_provider;
};

TEST_F(RelayTunnelTest, AListingCrossesTheRelay) {
    ASSERT_TRUE(connectProvider()) << m_error.toStdString();
    const QVector<FileInfo> entries = m_provider->list(QStringLiteral("/share"), true);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().name(), QStringLiteral("hello.txt"));
    EXPECT_EQ(entries.first().size(), 20);
}

TEST_F(RelayTunnelTest, AFileRoundTripsThroughTheRelay) {
    ASSERT_TRUE(connectProvider()) << m_error.toStdString();

    const QByteArray payload = blob(120 * 1024, 5);
    FileHandle *out = m_provider->openWrite(QStringLiteral("/share/up.bin"), true);
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

    QFile landed(m_share + QStringLiteral("/up.bin"));
    ASSERT_TRUE(landed.open(QIODevice::ReadOnly));
    EXPECT_EQ(landed.readAll(), payload);
    landed.close();

    FileHandle *in = m_provider->openRead(QStringLiteral("/share/up.bin"));
    ASSERT_NE(in, nullptr);
    QByteArray got;
    QByteArray chunk(32 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 n = m_provider->read(in, chunk.data(), chunk.size());
        ASSERT_GE(n, 0);
        if (n == 0)
            break;
        got.append(chunk.constData(), int(n));
    }
    EXPECT_TRUE(m_provider->closeHandleStatus(in));
    EXPECT_EQ(got, payload);
}

} // namespace

#include "test_RelayTunnel.moc"
