#pragma once

#include <QString>
#include <QVector>

// A remote-server bookmark: everything needed to re-open a connection except
// the password, which is kept in the system keyring (libsecret) and looked up
// by id. Protocol is stored as an int matching GvfsMounter::Protocol so this
// struct doesn't have to depend on that header.
struct SavedConnection {
    QString id;                              // stable UUID; keyring key
    QString name;                            // user-facing label, e.g. "Home NAS"
    int protocol = 0;                        // GvfsMounter::Protocol as int
    QString host;
    int port = 0;
    QString user;
    QString remotePath = QStringLiteral("/");
    bool anonymous = false;
};

// Persists connection bookmarks: metadata lives in the app's INI file (the same
// FileCommander/config.ini Settings uses), passwords live in the login keyring
// via libsecret. Passwords never touch the INI. All calls are synchronous,
// which is fine for use from a modal dialog.
class ConnectionStore {
public:
    // Every saved bookmark, in insertion order.
    static QVector<SavedConnection> loadAll();

    // The bookmark with this id, or a default-constructed one (empty id) if
    // none exists.
    static SavedConnection load(const QString &id);

    // Upserts by id. When conn.id is empty a fresh UUID is assigned; the id of
    // the stored bookmark (new or existing) is returned.
    static QString save(const SavedConnection &conn);

    // Removes the bookmark and its keyring password. No-op if id is unknown.
    static void remove(const QString &id);

    // Password storage in the login keyring. storePassword returns false if the
    // keyring rejected the write; loadPassword returns an empty string when no
    // password is stored (or the keyring is unavailable).
    static bool storePassword(const QString &id, const QString &password);
    static QString loadPassword(const QString &id);
    static void clearPassword(const QString &id);
};
