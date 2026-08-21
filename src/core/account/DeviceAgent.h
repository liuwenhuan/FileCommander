#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AccountClient;
class QTimer;
class QWebSocket;

// The presence socket: one long-lived WebSocket to the account server's
// /v1/agent endpoint, for as long as this install is signed in.
//
// It carries no file bytes. It exists so the server knows this device is
// reachable and at which addresses, and so the server can push a ticket here
// when another device of the same account wants to connect -- the peer's
// FileShareServer then accepts that ticket without a round trip back to the
// server on every request.
//
// Reconnect follows the cadence NetworkSession already settled on (12s connect
// timeout, 1/2/4/8/8s backoff), but unlike a file-transfer session it never
// gives up: a device that stopped retrying would show as permanently offline to
// the rest of the account with nothing to prompt a retry.
class DeviceAgent : public QObject {
    Q_OBJECT

public:
    explicit DeviceAgent(AccountClient *client, QObject *parent = nullptr);
    ~DeviceAgent() override;

    // Opens the socket and keeps it open. Safe to call twice.
    void start();

    // Closes it and stops reconnecting.
    void stop();

    bool isConnected() const;

    // The port the local FileShareServer listens on, reported to the server so
    // a peer knows where to dial. Re-sends the hello when already connected, so
    // a share that starts after the socket does is still announced.
    void setSharePort(quint16 port);

    // The names of the folders this device is actually serving, so a peer can
    // show them before it browses. Reported with the hello, like the port.
    void setShareNames(const QStringList &names);

    // This machine's LAN addresses, in the order a peer should try them.
    // Loopback, link-local and down interfaces are left out, as is anything
    // containing a comma -- the server stores the list comma-separated.
    static QStringList localAddresses();

signals:
    void connected();
    void disconnected();
    // The server answered "ready", i.e. it has recorded this device's hello and
    // written last_seen -- so a device list fetched now would show us online.
    // Emitted on the initial hello and again whenever the share port changes,
    // which is what re-sends the hello.
    void announced();

    // The server pushed a ticket: `from` wants to connect to us. The caller
    // hands the ticket to FileShareServer, which will then accept it, and uses
    // the session id to meet the peer on the relay if it cannot reach us
    // directly -- we cannot tell which it will be, so both are prepared.
    void ticketOffered(const QString &sessionId, const QString &ticket, const QString &from,
                       int expiresIn);

    // The server told us another device of this account came online or went
    // offline. The caller re-fetches the device list so the peer's row flips
    // in real time rather than on the next manual refresh.
    void presenceChanged(const QString &deviceId, bool online);

private:
    void openSocket();
    void scheduleReconnect();
    void sendHello();
    void onTextMessage(const QString &text);

    AccountClient *m_client;
    QWebSocket *m_socket;
    QTimer *m_heartbeat;
    QTimer *m_reconnect;
    quint16 m_sharePort = 0;
    QStringList m_shareNames;
    int m_attempt = 0;
    bool m_wanted = false; // start() called and stop() not
};
