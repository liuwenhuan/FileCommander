#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class FilePanel;
class NetworkSession;

// One live server connection as the folder tree sees it.
struct RegisteredConnection {
    QString connectionId; // "smb://user@host"; identity, and the registry key
    QString label;        // "user@host", snapshotted at registration
    QString scheme;       // "smb" / "sftp" / "ftp" / "webdav"
    QString basePath;     // topmost visible directory (provider-relative)
    // The session is held weakly on purpose: the connection's real owner is the
    // tab that made it. When that tab closes or disconnects, the session dies
    // and the entry expires on its own -- no reference counting, and no way for
    // the registry to keep a dead connection (or a live one nobody wants) alive.
    std::weak_ptr<NetworkSession> session;
    // Which panel's tab owns this connection. A tree only lets the user activate
    // its own panel's connections; the others are shown but greyed.
    const FilePanel *owner = nullptr;
};

// Tracks every live network connection across both panels, so each panel's
// folder tree can show a root per connection. MainWindow owns the single
// instance and injects it into both panels.
//
// Registration happens where a connection's identity is stamped onto its tab.
// Entries whose session has expired are pruned lazily on the next query, so an
// abandoned connection cannot leave a dangling root behind.
class NetworkTreeRegistry : public QObject {
    Q_OBJECT

public:
    explicit NetworkTreeRegistry(QObject *parent = nullptr);

    // Adds or updates the entry for `conn.connectionId`. Re-registering the same
    // id (e.g. the label firming up once the link is established) replaces the
    // entry in place rather than duplicating it. Emits changed() only when
    // something the tree renders actually differs.
    void registerConnection(const RegisteredConnection &conn);

    // Explicitly drops a connection (tab disconnected / closed). Expiry alone
    // would eventually catch it, but a deliberate disconnect should update the
    // tree immediately rather than at the next unrelated query.
    void unregisterConnection(const QString &connectionId);

    // Live connections, with expired entries pruned. Non-const in effect (it
    // prunes), so callers get a snapshot by value.
    QVector<RegisteredConnection> connections();

    // Looks up the live session for a connection, or null when it has expired.
    std::shared_ptr<NetworkSession> sessionFor(const QString &connectionId);

signals:
    // The set of live connections changed: trees should rebuild their roots.
    void changed();

private:
    // Removes entries whose session has expired. Returns true if it removed any.
    bool pruneExpired();

    QVector<RegisteredConnection> m_connections;
};
