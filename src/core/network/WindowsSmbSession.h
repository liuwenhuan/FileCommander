#pragma once

#include <QMutex>
#include <QSet>
#include <QString>

// Owns the Windows-side SMB credentials and the network connections opened with
// them, and is the one place that decides what a Windows networking status code
// actually means for the caller.
//
// That classification matters more than it looks: the difference between "the
// server wants a password" and "the server is not there" decides whether the UI
// raises a login prompt or reports a dead host, and Windows expresses both as a
// bare DWORD among several dozen.
class WindowsSmbSession {
public:
    // Outcome of an attempt to reach a server or a share.
    //
    // AuthRequired is deliberately distinct from Failed: re-dialling an
    // anonymous connect that was rejected for credentials only gets rejected
    // again, so the caller must prompt rather than back off and retry.
    enum class Result { Connected, AuthRequired, Failed };

    WindowsSmbSession() = default;
    ~WindowsSmbSession();

    void setCredentials(const QString &user, const QString &password, bool anonymous);

    // Establishes a session with `host` itself rather than with one of its
    // shares, by connecting to its IPC$ pipe -- the standard Windows way to
    // authenticate to a server without mounting anything. This is what lets a
    // wrong or missing password fail at connect time, where the UI can act on
    // it, instead of surfacing later as an inexplicably empty listing.
    Result connectToServer(const QString &host, QString *error);

    // Connects to one share, e.g. "\\server\share".
    Result ensureConnected(const QString &uncShare, QString *error);

    // Whether any session opened through this object is currently held. Lets
    // "is the connection live" be a real answer instead of a test of whether a
    // host-name string happens to be non-empty.
    bool holdsConnection() const;

    // Whether the most recent connectToServer adopted a session Windows already
    // had, instead of opening one. See FileProvider::reusedExistingSession().
    bool lastConnectBorrowed() const;

    // The account the existing connection to `host` was made under, or empty if
    // there is none or Windows will not say. Read from the logon session's own
    // connection table, so it reports the identity actually in force -- not the
    // one we asked for and did not get.
    static QString existingSessionUser(const QString &host);

    void disconnectOwned();

    // Maps a Win32/WNet status to a Result. Exposed because this mapping is the
    // part of the class most likely to be wrong and it is pure, so it can be
    // tested without a server.
    static Result classify(unsigned long status);
    // The Windows message for a status code, or a readable fallback.
    static QString describe(unsigned long status);

private:
    Q_DISABLE_COPY(WindowsSmbSession)

    Result connectTarget(const QString &uncTarget, QString *error);

    mutable QMutex m_mutex;
    QString m_user;
    QString m_password;
    bool m_anonymous = false;
    // Connections this object created and must therefore cancel on the way out.
    QSet<QString> m_ownedConnections;
    // Targets Windows was already connected to when we asked (ERROR_ALREADY_
    // ASSIGNED / ERROR_SESSION_CREDENTIAL_CONFLICT). They count as connected but
    // must NOT be cancelled: they are not ours, and other applications -- or
    // Explorer -- may be relying on them.
    QSet<QString> m_borrowedConnections;
    bool m_lastConnectBorrowed = false;
};
