#pragma once

#include <QObject>
#include <QString>

class QThread;

// The tunnel used when two devices cannot reach each other directly: it turns
// the account server's relay WebSocket back into an ordinary TCP port.
//
// It exists for one reason only -- so CurlWebDavProvider and FileShareServer
// keep working unchanged over the relay. Neither of them learns anything about
// WebSockets; one dials a loopback port, the other is dialled on its own.
//
// One WebSocket carries one TCP connection, because the accessing side opens
// several in parallel (CurlWebDavProvider reports four read channels). The
// serving side therefore parks a small pool of sockets on the relay, so a
// connection its peer makes finds one waiting rather than a round trip away.
//
// Like FileShareServer, everything runs on its own thread: a transfer that
// shared the GUI thread would stall the window for its whole length.
class RelayTunnel : public QObject {
    Q_OBJECT

public:
    explicit RelayTunnel(QObject *parent = nullptr);
    ~RelayTunnel() override;

    // Accessing side. Listens on 127.0.0.1 and returns the port, or 0 if it
    // could not bind. Every connection made to that port opens a relay socket
    // and comes out at the peer's FileShareServer. Synchronous, because the
    // caller needs the port to build the provider's connect closure.
    quint16 listenLocal(const QString &relayUrl);

    // Serving side. Parks `channels` sockets on the relay; each one, when its
    // peer arrives, opens a connection to 127.0.0.1:`localPort` -- the local
    // FileShareServer -- and parks a replacement.
    void serveLocal(const QString &relayUrl, quint16 localPort, int channels = 4);

private:
    QThread *m_thread;
    // Owns the sockets and lives on m_thread. Typed as a plain QObject because
    // the class is private to the .cpp; it is driven through queued invocations
    // by name, so worker-owned state needs no lock.
    QObject *m_worker;
};
