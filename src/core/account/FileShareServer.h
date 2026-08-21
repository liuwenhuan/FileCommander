#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AccountClient;
class QThread;

// The serving half of device-to-device transfer: a minimal WebDAV server over
// the folders the user chose to share.
//
// WebDAV rather than a private protocol because the accessing side then needs
// no new code at all -- CurlWebDavProvider already implements the whole
// FileProvider contract (listing, streaming reads with resume, uploads, move,
// mkdir, delete) against a real server, so the peer just points it at this port.
// Only the verbs that provider actually sends are implemented: OPTIONS,
// PROPFIND (Depth 0 and 1), GET, HEAD, PUT, MKCOL, DELETE and MOVE.
//
// Every connection is TLS, with the device's own self-signed certificate from
// ShareIdentity; the peer checks it by pin rather than by CA or hostname. That
// is what keeps the relay operator -- who sees every byte when the LAN route
// does not work -- from reading any of them.
//
// Authentication is HTTP Basic, with a ticket as the password. Tickets come
// from the account server over DeviceAgent's socket, so a request can be judged
// here without a round trip. No ticket means 401, always: a machine that merely
// guessed the port must not be able to read a single byte.
//
// Everything runs on its own thread -- a file server that shared the GUI thread
// would stall the window for the length of a transfer.
class FileShareServer : public QObject {
    Q_OBJECT

public:
    // `client` has no default on purpose: the share exists for the account's
    // other devices, so signing out has to close the port, and wiring that here
    // is what keeps it true no matter which call site does the signing out. A
    // test driving the server on its own passes an explicit nullptr, which is
    // then visible in review rather than hidden in a default argument.
    explicit FileShareServer(AccountClient *client, QObject *parent = nullptr);
    ~FileShareServer() override;

    // Absolute paths of the folders to serve. Each appears at the root under
    // its own basename; a name collision gets a numeric suffix. Nothing outside
    // them is reachable, including through a symlink that points out.
    void setSharedFolders(const QStringList &folders);

    // Accepts `ticket` as a password until it expires.
    void addTicket(const QString &ticket, int ttlSeconds);

    // Starts listening on all interfaces. Port 0 picks a free one, which is the
    // normal case -- the port is reported to the account server, so it never
    // has to be a fixed number. Answers with started() or failed().
    void start(quint16 port = 0);
    void stop();

    quint16 port() const { return m_port; }
    bool isRunning() const { return m_port != 0; }

signals:
    void started(quint16 port);
    void failed(const QString &error);
    void stopped();

private slots:
    // The worker owns the listener, so the port is only knowable once it is
    // bound; these mirror it back for port()/isRunning().
    void rememberPort(quint16 port);
    void forgetPort();

private:
    QThread *m_thread;
    // The worker that owns the QTcpServer, and lives on m_thread. Typed as a
    // plain QObject because the class is private to the .cpp; it is driven
    // through queued invocations by name, so worker-owned state needs no lock.
    QObject *m_worker;
    quint16 m_port = 0;
};
