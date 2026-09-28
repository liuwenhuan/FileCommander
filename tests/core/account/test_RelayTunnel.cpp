#include <gtest/gtest.h>

#include <QDir>
#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include "account/FileShareServer.h"
#include "account/RelayTunnel.h"
#include "account/ShareIdentity.h"
#include "network/CurlWebDavProvider.h"
#include "operations/FileOperations.h"
#include "filesystem/LocalFileProvider.h"

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

    int startSecure(const QByteArray &certPem, const QByteArray &keyPem) {
        m_server = new QWebSocketServer(QStringLiteral("relay"),
                                        QWebSocketServer::SecureMode, this);
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(QSslCertificate(certPem, QSsl::Pem));
        config.setPrivateKey(QSslKey(keyPem, QSsl::Ec, QSsl::Pem));
        m_server->setSslConfiguration(config);
        if (!m_server->listen(QHostAddress::LocalHost, 0))
            return 0;
        connect(m_server, &QWebSocketServer::newConnection, this, &FakeRelay::onConnection);
        return m_server->serverPort();
    }

    int parked() const { return m_parked.size(); }

    int startRawSink() {
        m_rawSink = new QTcpServer(this);
        if (!m_rawSink->listen(QHostAddress::LocalHost, 0))
            return 0;
        connect(m_rawSink, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_rawSink->nextPendingConnection()) {
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    m_rawReceived.append(socket->readAll());
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return m_rawSink->serverPort();
    }

    qint64 rawReceivedSize() const { return m_rawReceived.size(); }
    QByteArray rawReceived() const { return m_rawReceived; }
    void setForwardDelayMs(int delay) { m_forwardDelayMs = delay; }

private:
    void onConnection() {
        QWebSocket *socket = m_server->nextPendingConnection();
        socket->setParent(this);
        connect(socket, &QWebSocket::binaryMessageReceived, this,
                [this, socket](const QByteArray &bytes) {
                    if (m_forwardDelayMs > 0)
                        QThread::msleep(static_cast<unsigned long>(m_forwardDelayMs));
                    if (QWebSocket *peer = m_peers.value(socket))
                        peer->sendBinaryMessage(bytes);
                    else
                        m_pending[socket].append(bytes);
                });
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString &text) {
                    if (text != QLatin1String("{\"type\":\"eof\"}"))
                        return;
                    socket->setProperty("eof", true);
                    QWebSocket *peer = m_peers.value(socket);
                    if (!peer || !peer->property("eof").toBool())
                        return;
                    m_peers.remove(socket);
                    m_peers.remove(peer);
                    closeWhenDrained(peer);
                    closeWhenDrained(socket);
                });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            if (QWebSocket *peer = m_peers.take(socket)) {
                m_peers.remove(peer);
                closeWhenDrained(peer);
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

    void closeWhenDrained(QWebSocket *socket) {
        QObject::connect(socket, &QWebSocket::bytesWritten, socket, [socket](qint64) {
            if (socket->bytesToWrite() == 0)
                QTimer::singleShot(0, socket, [socket] { socket->close(); });
        });
        socket->flush();
        if (socket->bytesToWrite() == 0)
            QTimer::singleShot(0, socket, [socket] { socket->close(); });
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
    QTcpServer *m_rawSink = nullptr;
    QByteArray m_rawReceived;
    int m_forwardDelayMs = 0;
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
        QObject::connect(&m_relayThread, &QThread::finished, m_relay,
                         &QObject::deleteLater);
        int relayPort = 0;
        QMetaObject::invokeMethod(m_relay, "start", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(int, relayPort));
        ASSERT_NE(relayPort, 0);
        // No ticket in the URL: it travels as an Authorization header set by
        // RelayTunnel, the same way the real client sends it.
        m_relayUrl = QStringLiteral("ws://127.0.0.1:%1/v1/relay/session").arg(relayPort);

        m_server = new FileShareServer(nullptr);
        m_server->setSharedFolders({m_share});
        m_server->addTicket(QString::fromLatin1(kTicket), 300);
        QSignalSpy up(m_server, &FileShareServer::started);
        m_server->start();
        ASSERT_TRUE(up.wait(5000));
        const quint16 sharePort = quint16(up.first().first().toUInt());

        m_serving = new RelayTunnel;
        m_serving->serveLocal(m_relayUrl, QString::fromLatin1(kTicket), sharePort, 4);
        // The accessing side must find a socket already parked; otherwise the
        // first request races the pool coming up.
        ASSERT_TRUE(waitForParked(4));

        m_accessing = new RelayTunnel;
        m_localPort = m_accessing->listenLocal(m_relayUrl, QString::fromLatin1(kTicket));
        ASSERT_NE(m_localPort, 0);
    }

    void TearDown() override {
        m_provider.reset();
        delete m_accessing;
        delete m_serving;
        delete m_server;
        m_relayThread.quit();
        m_relayThread.wait();
        m_relay = nullptr;
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

    const QByteArray payload = blob(2 * 1024 * 1024 + 123, 5);
    FileHandle *out = m_provider->openWrite(QStringLiteral("/share/up.bin"), true);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->receiverProgressSupported());
    m_provider->setExpectedWriteSize(out, payload.size());
    qint64 sent = 0;
    bool checkedConfirmation = false;
    while (sent < payload.size()) {
        const qint64 n = m_provider->write(out, payload.constData() + sent,
                                           qMin<qint64>(32 * 1024, payload.size() - sent));
        ASSERT_GT(n, 0);
        sent += n;
        if (!checkedConfirmation && sent >= 1024 * 1024) {
            qint64 confirmed = -1;
            for (int attempt = 0; attempt < 20 && confirmed <= 0; ++attempt) {
                confirmed = out->receiverConfirmedBytes();
                if (confirmed <= 0)
                    QThread::msleep(50);
            }
            EXPECT_GT(confirmed, 0);
            EXPECT_LT(confirmed, payload.size());
            checkedConfirmation = true;
        }
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

TEST(RelayTunnelFlowControlTest, ASlowRelayEventuallyDrainsLargeUpload) {
    const ShareIdentity::Identity identity = ShareIdentity::generate();
    ASSERT_TRUE(identity.isValid());
    const QSslConfiguration oldSsl = QSslConfiguration::defaultConfiguration();
    QSslConfiguration trustedSsl = oldSsl;
    trustedSsl.setCaCertificates(QSslCertificate::fromData(identity.certPem, QSsl::Pem));
    trustedSsl.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSslConfiguration::setDefaultConfiguration(trustedSsl);
    struct RestoreSsl {
        QSslConfiguration value;
        ~RestoreSsl() { QSslConfiguration::setDefaultConfiguration(value); }
    } restoreSsl{oldSsl};

    QThread relayThread;
    relayThread.start();
    struct StopThread {
        QThread &thread;
        ~StopThread() { thread.quit(); thread.wait(); }
    } stopThread{relayThread};
    auto *relay = new FakeRelay;
    relay->moveToThread(&relayThread);
    QObject::connect(&relayThread, &QThread::finished, relay, &QObject::deleteLater);
    int relayPort = 0;
    int sinkPort = 0;
    QMetaObject::invokeMethod(relay, "startSecure", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(int, relayPort), Q_ARG(QByteArray, identity.certPem),
                              Q_ARG(QByteArray, identity.keyPem));
    QMetaObject::invokeMethod(relay, "startRawSink", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(int, sinkPort));
    ASSERT_NE(relayPort, 0);
    ASSERT_NE(sinkPort, 0);
    QMetaObject::invokeMethod(relay, "setForwardDelayMs", Qt::BlockingQueuedConnection,
                              Q_ARG(int, 10));

    const QString url = QStringLiteral("wss://127.0.0.1:%1/v1/relay/session").arg(relayPort);
    RelayTunnel serving;
    serving.serveLocal(url, QString::fromLatin1(kTicket), quint16(sinkPort), 1);
    RelayTunnel accessing;
    const quint16 localPort = accessing.listenLocal(url, QString::fromLatin1(kTicket));
    ASSERT_NE(localPort, 0);

    QTcpSocket sender;
    sender.setProxy(QNetworkProxy::NoProxy);
    sender.connectToHost(QHostAddress::LocalHost, localPort);
    ASSERT_TRUE(sender.waitForConnected(5000));
    const QByteArray payload = blob(64 * 1024 * 1024, 19);
    ASSERT_EQ(sender.write(payload), payload.size());
    QElapsedTimer clock;
    clock.start();
    qint64 receivedSize = 0;
    while (clock.elapsed() < 60000) {
        QMetaObject::invokeMethod(relay, "rawReceivedSize", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(qint64, receivedSize));
        if (receivedSize == payload.size())
            break;
        sender.waitForBytesWritten(100);
    }
    QByteArray received;
    QMetaObject::invokeMethod(relay, "rawReceived", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(QByteArray, received));
    EXPECT_EQ(received, payload);
    sender.disconnectFromHost();
}

TEST_F(RelayTunnelTest, BenchmarkInstallerSingleStreamOverRelay) {
    const QString input = QString::fromLocal8Bit(qgetenv("FC_BENCH_FILE"));
    if (input.isEmpty())
        GTEST_SKIP() << "Set FC_BENCH_FILE to run the opt-in real-file benchmark";
    const QFileInfo original(input);
    ASSERT_TRUE(original.isFile());
    ASSERT_GT(original.size(), 20LL * 1024 * 1024);

    auto digest = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd())
            hash.addData(file.read(1024 * 1024));
        return hash.result();
    };
    const QByteArray expectedHash = digest(input);
    ASSERT_FALSE(expectedHash.isEmpty());

    const QString source = m_dir.filePath(QStringLiteral("single-pixeloffice.exe"));
    ASSERT_TRUE(QFile::copy(input, source));
    ASSERT_TRUE(connectProvider()) << m_error.toStdString();

    FileOperations operations;
    QString error;
    QElapsedTimer clock;
    clock.start();
    ASSERT_TRUE(operations.copyAcrossProviders(LocalFileProvider::instance(), {source},
                                               m_provider.get(), QStringLiteral("/share"),
                                               false, nullptr, &error))
        << error.toStdString();
    const qint64 elapsedMs = clock.elapsed();
    const QString received = m_share + QLatin1Char('/') + QFileInfo(source).fileName();
    EXPECT_EQ(digest(received), expectedHash);
    qInfo().noquote() << "relay benchmark single" << original.size() << "bytes"
                      << elapsedMs << "ms"
                      << double(original.size()) * 1000.0 / (1024 * 1024 * elapsedMs)
                      << "MiB/s";
    m_provider.reset();
}

} // namespace

#include "test_RelayTunnel.moc"
