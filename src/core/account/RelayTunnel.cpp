#include "RelayTunnel.h"

#include <QHostAddress>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace {

// A parked socket the server never paired is sent away after a minute (see
// PARK_SECONDS in the server's relay.rs); park a replacement rather than
// leaving the pool one short, but not in a tight loop if the relay is down.
constexpr int kReparkDelayMs = 1000;

// How long a drained pipe waits for the peer's close/disconnect signal before
// falling back, never a deadline for payload still queued in Qt.
constexpr int kCloseFallbackMs = 5000;
const char kEndMessage[] = "{\"type\":\"eof\"}";

// Back-pressure: once a side has this many bytes queued in its own write
// buffer, the pipe stops feeding it and leaves the data upstream, so TCP/WS
// flow control stalls the sender instead of the relay buffering a whole file
// in RAM (which is what made the transfer progress race ahead of the wire).
constexpr qint64 kHighWaterBytes = 1024 * 1024;
// Forward in bounded chunks so no single WebSocket frame is ever the whole file.
constexpr qint64 kChunkBytes = 64 * 1024;

// Joins one relay WebSocket to one TCP connection and copies bytes both ways
// until either end goes away. Either end may still be connecting when the pipe
// is built, so both directions buffer until their side is up -- and both are
// gated by the receiving side's pending bytes, so a slow peer stalls the sender
// through real flow control rather than through unbounded buffering.
class Pipe : public QObject {
    Q_OBJECT

public:
    Pipe(QWebSocket *ws, QTcpSocket *tcp, QObject *parent)
        : QObject(parent), m_ws(ws), m_tcp(tcp) {
        ws->setParent(this);
        tcp->setParent(this);
        // Bound the loopback socket's own read buffer. Without this the socket
        // keeps draining curl's send into an unbounded internal buffer even
        // while the pipe is not forwarding, so TCP flow control never reaches
        // curl and its upload progress races ahead of the relay. A capped
        // buffer means the OS receive window closes once the pipe stalls, which
        // is what actually slows the sender down.
        tcp->setReadBufferSize(kHighWaterBytes);

        connect(ws, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray &bytes) {
            m_pendingToTcp.append(bytes);
            pumpToTcp();
        });
        connect(ws, &QWebSocket::connected, this, [this] { pumpToWebSocket(); });
        connect(ws, &QWebSocket::bytesWritten, this, [this](qint64) {
            pumpToWebSocket();
            sendEndIfDrained();
            startEndFallbackIfDrained();
        });
        connect(ws, &QWebSocket::disconnected, this, &Pipe::finish);
        // String-based, because the typed overload of error() is deprecated in
        // favour of a signal that does not exist in older Qt 5.
        connect(ws, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(finish()));

        connect(tcp, &QTcpSocket::readyRead, this, [this] { pumpToWebSocket(); });
        connect(tcp, &QTcpSocket::connected, this, [this] { pumpToTcp(); });
        connect(tcp, &QTcpSocket::bytesWritten, this, [this](qint64) {
            pumpToTcp();
            disconnectTcpIfDrained();
        });
        connect(tcp, &QTcpSocket::disconnected, this, &Pipe::finish);
        connect(tcp, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(finish()));
    }

private slots:
    void finish() {
        if (m_done)
            return;
        m_done = true;

        if (sender() == m_tcp) {
            // RemoteHostClosedError can arrive while the socket still has the
            // last TLS record buffered. Forward it, then keep this pipe alive
            // until QWebSocket reports those bytes written.
            pumpToWebSocket();
            if (m_ws->state() != QAbstractSocket::ConnectedState) {
                deleteLater();
                return;
            }
            m_sendEndWhenDrained = true;
            connect(m_ws, &QWebSocket::disconnected, this, &QObject::deleteLater);
            sendEndIfDrained();
            return;
        }

        // The WebSocket has ended; finish draining its last frames into the
        // local socket rather than closing the already-invalid native socket again.
        if (m_tcp->state() != QAbstractSocket::ConnectedState) {
            m_tcp->close();
            deleteLater();
            return;
        }
        // The last TLS record may still be in QTcpSocket's write buffer. Wait
        // until both queues drain before asking Qt for a graceful disconnect.
        connect(m_tcp, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        m_disconnectTcpWhenDrained = true;
        pumpToTcp();
        // Qt 5 queues QWebSocket's readyRead parser. Queue this check behind it
        // so a final complete binary frame can reach binaryMessageReceived
        // before an apparently empty local queue is disconnected.
        QMetaObject::invokeMethod(this, [this] {
            pumpToTcp();
            disconnectTcpIfDrained();
        }, Qt::QueuedConnection);
    }

private:
    qint64 webSocketBytesToWrite() const {
        qint64 bytes = m_ws->bytesToWrite();
        // For WSS, QWebSocket::bytesToWrite() is QSslSocket's unencrypted
        // queue. Ciphertext waiting on the TCP socket is a separate public
        // counter on the QSslSocket child that QWebSocket creates in open().
        if (auto *ssl = m_ws->findChild<QSslSocket *>(QString(), Qt::FindDirectChildrenOnly))
            bytes += ssl->encryptedBytesToWrite();
        return bytes;
    }

    void sendEndIfDrained() {
        if (!m_sendEndWhenDrained || webSocketBytesToWrite() > 0)
            return;
        m_sendEndWhenDrained = false;
        m_waitEndWrite = true;
        m_ws->sendTextMessage(QString::fromLatin1(kEndMessage));
        m_ws->flush();
        startEndFallbackIfDrained();
    }

    void startEndFallbackIfDrained() {
        if (!m_waitEndWrite || webSocketBytesToWrite() > 0)
            return;
        m_waitEndWrite = false;
        // EOF itself is now out of Qt's plaintext and TLS ciphertext queues.
        // Wait only for an old relay that ignores the control message.
        QTimer::singleShot(kCloseFallbackMs, this, [this] {
            if (m_ws->state() != QAbstractSocket::UnconnectedState)
                m_ws->close();
            deleteLater();
        });
    }

    void disconnectTcpIfDrained() {
        if (!m_disconnectTcpWhenDrained || !m_pendingToTcp.isEmpty() ||
            m_tcp->bytesToWrite() > 0)
            return;
        m_disconnectTcpWhenDrained = false;
        m_tcp->disconnectFromHost();
        // At this point no payload remains queued; only bound a missing
        // disconnected signal rather than timing the data drain itself.
        QTimer::singleShot(kCloseFallbackMs, this, &QObject::deleteLater);
    }

    // Local socket -> relay. While the relay socket is not yet up, the bytes
    // park in m_toWs; once it is, they are forwarded in chunks but only while
    // the relay's own send queue is below the high-water mark. When it is not,
    // the unread bytes stay in the local socket's receive buffer, which is what
    // pushes TCP flow control back onto curl.
    void pumpToWebSocket() {
        if (m_ws->state() != QAbstractSocket::ConnectedState) {
            m_toWs.append(m_tcp->readAll());
            return;
        }
        if (!m_toWs.isEmpty()) {
            m_ws->sendBinaryMessage(m_toWs);
            m_toWs.clear();
        }
        // Bound both QSslSocket queues. This covers plaintext waiting for TLS
        // and ciphertext waiting for TCP, so a slow relay applies real flow
        // control instead of accumulating an uncounted encrypted backlog.
        while (webSocketBytesToWrite() < kHighWaterBytes && m_tcp->bytesAvailable() > 0) {
            const qint64 n = qMin<qint64>(m_tcp->bytesAvailable(), kChunkBytes);
            const QByteArray bytes = m_tcp->read(n);
            if (bytes.isEmpty())
                break;
            m_ws->sendBinaryMessage(bytes);
        }
    }

    // Relay -> local socket, the mirror image: frames are written as the local
    // socket drains, never dropped, and paused while its write queue is full.
    void pumpToTcp() {
        if (m_tcp->state() != QAbstractSocket::ConnectedState)
            return;
        while (!m_pendingToTcp.isEmpty() && m_tcp->bytesToWrite() < kHighWaterBytes) {
            const QByteArray &bytes = m_pendingToTcp.first();
            const qint64 n = m_tcp->write(bytes);
            if (n <= 0)
                return;
            if (n < bytes.size())
                m_pendingToTcp.first() = bytes.mid(static_cast<int>(n));
            else
                m_pendingToTcp.removeFirst();
        }
    }

    QWebSocket *m_ws;
    QTcpSocket *m_tcp;
    QByteArray m_toWs;                  // bytes waiting for the relay socket to come up
    QList<QByteArray> m_pendingToTcp;   // frames waiting for room in the local socket
    bool m_sendEndWhenDrained = false;
    bool m_waitEndWrite = false;
    bool m_disconnectTcpWhenDrained = false;
    bool m_done = false;
};

} // namespace

// Owns the listener and the parked sockets, and lives on the tunnel's thread.
class RelayWorker : public QObject {
    Q_OBJECT

public:
    ~RelayWorker() override { shutdown(); }

public slots:
    int startListen(const QString &relayUrl, const QString &ticket) {
        if (m_listener)
            return m_listener->serverPort();
        m_url = relayUrl;
        m_ticket = ticket;
        m_listener = new QTcpServer(this);
        // Same reason FileShareServer needs it: an $all_proxy in the
        // environment otherwise makes listen() try to bind through SOCKS.
        m_listener->setProxy(QNetworkProxy::NoProxy);
        connect(m_listener, &QTcpServer::newConnection, this, &RelayWorker::onLocalConnection);
        // Loopback only. The relay is this machine's own business; a peer
        // reaches the share through the account server, never through here.
        if (!m_listener->listen(QHostAddress::LocalHost, 0)) {
            delete m_listener;
            m_listener = nullptr;
            return 0;
        }
        return m_listener->serverPort();
    }

    void startServe(const QString &relayUrl, const QString &ticket, int localPort, int channels) {
        m_url = relayUrl;
        m_ticket = ticket;
        m_localPort = static_cast<quint16>(localPort);
        for (int i = 0; i < channels; ++i)
            park();
    }

    void shutdown() {
        m_stopping = true;
        delete m_listener; // closes the listener; pipes are separate children
        m_listener = nullptr;
        for (QObject *child : children())
            child->deleteLater();
    }

private:
    QWebSocket *openRelay(const char *role) {
        auto *ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        ws->setProxy(QNetworkProxy::NoProxy);
        QUrl url(m_url);
        QString query = url.query();
        if (!query.isEmpty())
            query += QLatin1Char('&');
        url.setQuery(query + QLatin1String("role=") + QLatin1String(role));
        // The ticket is the credential for this socket and must not ride in the
        // URL (access logs); send it as a header instead.
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Bearer " + m_ticket.toUtf8());
        ws->open(request);
        return ws;
    }

    // Accessing side: curl dialled the loopback port, so the TCP end already
    // exists and the relay socket is opened to meet it.
    void onLocalConnection() {
        while (QTcpSocket *socket = m_listener->nextPendingConnection())
            new Pipe(openRelay("connect"), socket, this);
    }

    // Serving side: one socket waiting on the relay for a peer to turn up.
    void park() {
        if (m_stopping)
            return;
        QWebSocket *ws = openRelay("accept");
        connect(ws, &QWebSocket::textMessageReceived, this, [this, ws](const QString &text) {
            // "paired" is the relay saying this socket now carries a
            // connection; only then is there anything to serve.
            if (!text.contains(QLatin1String("paired")) || ws->property("paired").toBool())
                return;
            ws->setProperty("paired", true);
            auto *tcp = new QTcpSocket;
            tcp->setProxy(QNetworkProxy::NoProxy);
            new Pipe(ws, tcp, this);
            tcp->connectToHost(QHostAddress::LocalHost, m_localPort);
            park(); // keep the pool the size it was
        });
        connect(ws, &QWebSocket::disconnected, this, [this, ws] {
            if (ws->property("paired").toBool())
                return; // adopted; the Pipe owns it now
            ws->deleteLater();
            QTimer::singleShot(kReparkDelayMs, this, [this] { park(); });
        });
    }

    QTcpServer *m_listener = nullptr;
    QString m_url;
    QString m_ticket;
    quint16 m_localPort = 0;
    bool m_stopping = false;
};

RelayTunnel::RelayTunnel(QObject *parent)
    : QObject(parent), m_thread(new QThread(this)), m_worker(new RelayWorker) {
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

RelayTunnel::~RelayTunnel() {
    QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

quint16 RelayTunnel::listenLocal(const QString &relayUrl, const QString &ticket) {
    int port = 0;
    QMetaObject::invokeMethod(m_worker, "startListen", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(int, port), Q_ARG(QString, relayUrl),
                              Q_ARG(QString, ticket));
    return static_cast<quint16>(port);
}

void RelayTunnel::serveLocal(const QString &relayUrl, const QString &ticket, quint16 localPort,
                             int channels) {
    QMetaObject::invokeMethod(m_worker, "startServe", Qt::QueuedConnection,
                              Q_ARG(QString, relayUrl), Q_ARG(QString, ticket),
                              Q_ARG(int, static_cast<int>(localPort)), Q_ARG(int, channels));
}

#include "RelayTunnel.moc"
