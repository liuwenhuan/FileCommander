#pragma once

#include <functional>

#include <QObject>
#include <QStringList>
#include <QString>
#include <QVector>

// One machine signed in to the account, as the server reports it. Populated
// from GET /v1/devices; `id` is what a later transfer session is opened
// against, so it is the field the UI carries around, not the name.
struct AccountDeviceInfo {
    QString id;
    QString name;      // user-facing label, e.g. "work laptop"
    QString platform;  // "linux", "windows", ...
    bool online = false;
    bool self = false;         // this install
    QStringList lanAddresses;  // addresses to try before falling back to relay
    QString lastSeen;          // ISO-8601, empty when never seen
};

// One transfer session opened against a peer device, as POST /v1/session
// answers it. `ticket` is the short-lived password the peer's FileShareServer
// will accept; `peerLanAddresses` are the addresses to try before falling back
// to the relay.
struct AccountSession {
    QString sessionId;
    QString ticket;
    QStringList peerLanAddresses;
    quint16 peerPort = 0;
    // The peer's TLS pin, in curl's --pinnedpubkey form. Hand it to
    // CurlWebDavProvider::setPinnedPublicKey() before dialling: the peer serves
    // a self-signed certificate, so this is the only thing identifying it.
    QString peerPin;
    int expiresIn = 0;
};

// The signed-in account. Held by AccountClient; the access token is NOT part of
// it -- callers never need to see a token.
struct AccountInfo {
    QString email;
    QString deviceId; // this install's device id, assigned by the server
};

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// REST client for the FileCommander account server (see server/README.md):
// register, sign in, keep the session alive, list the account's other devices.
//
// Shaped after UpdateChecker: QNetworkAccessManager + JSON, server address from
// a compile-time default with an environment override, and every request emits
// exactly one signal -- including on timeout, so no caller can be left waiting
// on a server that connects and then says nothing.
//
// Token handling follows ConnectionStore's rule that secrets never reach the
// INI file: the refresh token lives in the login keyring, the access token
// lives in memory only, and config.ini holds nothing but the email, the device
// id and the server address.
class AccountClient : public QObject {
    Q_OBJECT

public:
    explicit AccountClient(QObject *parent = nullptr);
    ~AccountClient() override;

    // The API root this run will use: the compiled-in default
    // (-DFILECOMMANDER_ACCOUNT_API_URL at configure time) unless the
    // environment variable FILECOMMANDER_ACCOUNT_API_URL overrides it. Read
    // once per call, never cached, and never with a trailing slash.
    static QString apiUrl();

    // True when the effective URL is still the unreachable placeholder, i.e.
    // nobody has configured an account server for this build. Checked before
    // any request so the user is told "not configured" rather than shown a DNS
    // error.
    static bool apiUrlIsConfigured();

    // Overrides the API root for this instance only (tests, diagnostics).
    void setApiUrl(const QString &url);

    // How long to wait for a reply before giving up. Default 15s.
    void setTimeoutMs(int ms);

    // Creates an account. Emits registered() or requestFailed().
    void registerAccount(const QString &email, const QString &password);

    // Signs in and registers (or re-registers) this machine as a device.
    // `deviceId` re-claims an existing device row; pass the stored one so a
    // second sign-in does not add a second device. Emits loggedIn() or
    // requestFailed().
    void login(const QString &email, const QString &password,
               const QString &deviceName, const QString &deviceId = QString());

    // Restores a session from the keyring refresh token stored by a previous
    // login, without asking for the password again. Emits loggedIn() or
    // requestFailed(); the caller should treat a failure as "signed out".
    void restoreSession(const QString &email, const QString &deviceId);

    // Signs this device out: revokes the session server-side and deletes the
    // keyring refresh token. Emits loggedOut() once, even if the server could
    // not be reached -- the local session is gone either way.
    void logout();

    // Lists the account's devices. Emits devicesReady() or requestFailed().
    void fetchDevices();

    // Signs another device out of the account: its refresh token stops working
    // and it disappears from the device list. Emits deviceRemoved() (followed
    // by a fresh devicesReady()) or requestFailed(). Removing this device is
    // the caller's business, not ours -- it should log out instead, so the
    // local keyring entry goes with it.
    void removeDevice(const QString &deviceId);

    // Asks the server to open a transfer session against `deviceId`. The server
    // pushes the ticket to that device over its agent socket, so by the time
    // sessionReady() fires the peer already knows to accept it. Emits
    // sessionReady() or requestFailed().
    void openSession(const QString &deviceId);

    // The ws:// (or wss://) URL of the agent socket, without any credential --
    // the access token travels as an Authorization header (see
    // agentSocketRequest), never in the URL, so it cannot leak into server or
    // proxy access logs. Empty when not signed in. Read afresh on every
    // reconnect so a renewed token is picked up without wiring it through.
    QString agentSocketUrl() const;

    // The agent handshake request, Authorization header already set. An empty
    // URL means "not signed in", which DeviceAgent reads as "keep trying".
    QNetworkRequest agentSocketRequest() const;

    // WebSocket URL of the relay for `sessionId`. The ticket authenticates it
    // but is deliberately NOT in the URL: it travels as an Authorization header
    // set by RelayTunnel, which is passed the ticket separately. RelayTunnel
    // appends the role that says which side of the socket this is.
    QString relaySocketUrl(const QString &sessionId) const;

    bool isLoggedIn() const;
    AccountInfo account() const;

    // Deletes the refresh token this account keeps in the keyring. Called by
    // logout(); exposed because a caller that abandons an account (server gone,
    // credentials rotated) must be able to clean up without a round trip.
    static void forgetStoredSession(const QString &deviceId);

signals:
    void registered(const QString &email);
    void loggedIn(const AccountInfo &info);
    void loggedOut();
    void devicesReady(const QVector<AccountDeviceInfo> &devices);
    void sessionReady(const AccountSession &session);
    void deviceRemoved(const QString &deviceId);

    // Every failed request lands here, with a message already fit to show.
    void requestFailed(const QString &error);

private:
    enum class Verb { Get, Post, Delete };

    // Issues one request and calls `handler` exactly once with the finished
    // reply -- including when the request times out, in which case the reply
    // carries OperationCanceledError. `retryAfterRefresh` lets an authenticated
    // request that comes back 401 renew the access token and try again once;
    // the retry itself passes false so a server that always says 401 cannot
    // loop.
    void request(Verb verb, const QString &path, const QByteArray &body,
                 bool authenticated, std::function<void(QNetworkReply *)> handler,
                 bool retryAfterRefresh = true);

    // Turns a finished reply into a message worth showing, preferring the
    // server's own "detail" over Qt's generic network error.
    static QString errorText(QNetworkReply *reply, const QByteArray &body);

    // Stores the tokens from a login/refresh response. The refresh token goes
    // to the keyring, the access token stays in this object.
    bool acceptTokens(const QByteArray &body, const QString &email);

    QNetworkAccessManager *m_net;
    QString m_apiUrl;
    int m_timeoutMs = 15000;
    QString m_accessToken; // memory only, never persisted
    AccountInfo m_account;
};

// Both structs travel through queued signals (and through QSignalSpy in the
// tests), which needs them registered with the meta-object system.
Q_DECLARE_METATYPE(AccountDeviceInfo)
Q_DECLARE_METATYPE(AccountSession)
Q_DECLARE_METATYPE(AccountInfo)
