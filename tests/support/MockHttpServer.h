#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// An event-driven HTTP peer. Event-driven rather than blocking because
// QNetworkAccessManager lives on the test's own thread: a server that blocked
// waiting for a connection would deadlock against the client it is serving.
class MockHttpServer : public QObject {
public:
    struct Route {
        int status = 200;
        QByteArray contentType = "application/json";
        QByteArray body;
        int delayMs = 0;      // answer this late (exercises the client timeout)
        bool silent = false;  // accept the request and never answer at all
        bool dropAfterHeaders = false; // send the headers, then hang up mid-body
    };

    MockHttpServer() {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, &MockHttpServer::onConnection);
    }

    quint16 port() const { return m_server.serverPort(); }
    QString url(const QString &path) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path);
    }
    void setRoute(const QString &path, const Route &route) {
        m_routes.insert(path, QList<Route>{route});
    }
    // Answers successive hits on one path differently: hit N gets entry N, and
    // the last entry repeats forever. Lets a test drive a client through a
    // "401, then success after it renews" sequence without a second server.
    void setRouteSequence(const QString &path, const QList<Route> &routes) {
        m_routes.insert(path, routes);
    }
    int requestCount(const QString &path) const { return m_hits.value(path); }
    // Hits on every path starting with `prefix`. For a request whose query
    // string the test cannot predict -- a WebSocket handshake carries a fresh
    // access token in its URL -- this is what requestCount() cannot key on.
    int requestCountUnder(const QString &prefix) const {
        int total = 0;
        for (auto it = m_hits.constBegin(); it != m_hits.constEnd(); ++it)
            if (it.key().startsWith(prefix))
                total += it.value();
        return total;
    }
    QByteArray lastRequestHead() const { return m_lastHead; }

private:
    void onConnection() {
        QTcpSocket *sock = m_server.nextPendingConnection();
        if (!sock)
            return;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            m_pending[sock] += sock->readAll();
            if (!m_pending.value(sock).contains("\r\n\r\n"))
                return;
            const QByteArray head = m_pending.take(sock);
            m_lastHead = head;
            const QByteArray requestLine = head.left(head.indexOf("\r\n"));
            const QList<QByteArray> parts = requestLine.split(' ');
            const QString path = parts.size() > 1 ? QString::fromUtf8(parts.at(1)) : QString();
            m_hits[path] += 1;
            respond(sock, path, head);
        });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }

    void respond(QTcpSocket *sock, const QString &path, const QByteArray &head) {
        Q_UNUSED(head);
        if (!m_routes.contains(path)) {
            sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            sock->flush();
            return;
        }
        const QList<Route> &routes = m_routes[path];
        const Route route = routes.at(qMin(m_hits.value(path) - 1, routes.size() - 1));
        if (route.silent)
            return; // hold the connection open, say nothing

        auto send = [sock, route] {
            if (!sock || sock->state() != QAbstractSocket::ConnectedState)
                return;
            QByteArray head = "HTTP/1.1 " + QByteArray::number(route.status) + " X\r\n";
            head += "Content-Type: " + route.contentType + "\r\n";
            head += "Content-Length: " + QByteArray::number(route.body.size()) + "\r\n";
            head += "Connection: close\r\n\r\n";
            if (route.dropAfterHeaders) {
                sock->write(head);
                sock->flush();
                sock->abort();
                return;
            }
            // Write the whole reply and leave the socket open. Closing it here --
            // even through disconnectFromHost() -- resets the connection on
            // Windows before the peer reads the buffered response, so the client
            // sees a truncated reply or nothing at all. "Connection: close" plus
            // Content-Length lets the client finish on its own, and the client's
            // close then fires the disconnected handler that deletes the socket.
            sock->write(head + route.body);
            sock->flush();
        };

        if (route.delayMs > 0)
            QTimer::singleShot(route.delayMs, sock, send);
        else
            send();
    }

    QTcpServer m_server;
    QHash<QString, QList<Route>> m_routes;
    QHash<QString, int> m_hits;
    QHash<QTcpSocket *, QByteArray> m_pending;
    QByteArray m_lastHead;
};
