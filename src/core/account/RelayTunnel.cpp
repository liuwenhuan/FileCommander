#include "RelayTunnel.h"

#include <QHostAddress>
#include <QNetworkProxy>
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

// How long a closing pipe waits for what it has already written to reach the
// local side before giving up on it.
constexpr int kFlushTimeoutMs = 5000;

// Joins one relay WebSocket to one TCP connection and copies bytes both ways
// until either end goes away. Either end may still be connecting when the pipe
// is built, so both directions buffer until their side is up.
class Pipe : public QObject {
    Q_OBJECT

public:
    Pipe(QWebSocket *ws, QTcpSocket *tcp, QObject *parent)
        : QObject(parent), m_ws(ws), m_tcp(tcp) {
        ws->setParent(this);
        tcp->setParent(this);

        connect(ws, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray &bytes) {
            if (m_tcp->state() == QAbstractSocket::ConnectedState)
                m_tcp->write(bytes);
            else
                m_toTcp.append(bytes);
        });
        connect(ws, &QWebSocket::connected, this, [this] {
            if (m_toWs.isEmpty())
                return;
            m_ws->sendBinaryMessage(m_toWs);
            m_toWs.clear();
        });
        connect(ws, &QWebSocket::disconnected, this, &Pipe::finish);
        // String-based, because the typed overload of error() is deprecated in
        // favour of a signal that does not exist in older Qt 5.
        connect(ws, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(finish()));

        connect(tcp, &QTcpSocket::readyRead, this, [this] {
            const QByteArray bytes = m_tcp->readAll();
            if (m_ws->state() == QAbstractSocket::ConnectedState)
                m_ws->sendBinaryMessage(bytes);
            else
                m_toWs.append(bytes);
        });
        connect(tcp, &QTcpSocket::connected, this, [this] {
            if (m_toTcp.isEmpty())
                return;
            m_tcp->write(m_toTcp);
            m_toTcp.clear();
        });
        connect(tcp, &QTcpSocket::disconnected, this, &Pipe::finish);
        connect(tcp, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(finish()));
    }

private slots:
    void finish() {
        if (m_done)
            return;
        m_done = true;
        // The other half is told rather than left hanging: a half-open pipe
        // would keep a WebDAV connection alive with nothing behind it.
        m_ws->close();
        if (m_tcp->state() != QAbstractSocket::ConnectedState) {
            m_tcp->close();
            deleteLater();
            return;
        }
        // A last response is routinely still in the write buffer when the
        // server end hangs up -- FileShareServer answers 401 with
        // "Connection: close" and libcurl retries -- so disconnect gracefully
        // and outlive the flush, rather than closing on top of it.
        connect(m_tcp, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        QTimer::singleShot(kFlushTimeoutMs, this, &QObject::deleteLater);
        m_tcp->disconnectFromHost();
    }

private:
    QWebSocket *m_ws;
    QTcpSocket *m_tcp;
    QByteArray m_toWs;
    QByteArray m_toTcp;
    bool m_done = false;
};

} // namespace

// Owns the listener and the parked sockets, and lives on the tunnel's thread.
class RelayWorker : public QObject {
    Q_OBJECT

public:
    ~RelayWorker() override { shutdown(); }

public slots:
    int startListen(const QString &relayUrl) {
        if (m_listener)
            return m_listener->serverPort();
        m_url = relayUrl;
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

    void startServe(const QString &relayUrl, int localPort, int channels) {
        m_url = relayUrl;
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
        ws->open(url);
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

quint16 RelayTunnel::listenLocal(const QString &relayUrl) {
    int port = 0;
    QMetaObject::invokeMethod(m_worker, "startListen", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(int, port), Q_ARG(QString, relayUrl));
    return static_cast<quint16>(port);
}

void RelayTunnel::serveLocal(const QString &relayUrl, quint16 localPort, int channels) {
    QMetaObject::invokeMethod(m_worker, "startServe", Qt::QueuedConnection,
                              Q_ARG(QString, relayUrl), Q_ARG(int, static_cast<int>(localPort)),
                              Q_ARG(int, channels));
}

#include "RelayTunnel.moc"
