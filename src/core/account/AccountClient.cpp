#include "AccountClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include "CredentialStore.h"
#include "version.h"

namespace {

const char kUrlEnvVar[] = "FILECOMMANDER_ACCOUNT_API_URL";

// Keyring entry holding this install's refresh token. Keyed by device id so two
// accounts (or a re-registered install) never overwrite each other's token.
QString keyringId(const QString &deviceId) {
    return QStringLiteral("account:") + deviceId;
}

QJsonObject parseObject(const QByteArray &body) {
    return QJsonDocument::fromJson(body).object();
}

} // namespace

AccountClient::AccountClient(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {
    qRegisterMetaType<AccountInfo>();
    qRegisterMetaType<QVector<AccountDeviceInfo>>();
    qRegisterMetaType<AccountSession>();
}

AccountClient::~AccountClient() = default;

QString AccountClient::apiUrl() {
    const QByteArray override = qgetenv(kUrlEnvVar);
    QString url = override.isEmpty() ? QStringLiteral(TTC_ACCOUNT_API_URL)
                                     : QString::fromUtf8(override);
    url = url.trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return url;
}

bool AccountClient::apiUrlIsConfigured() {
    const QString url = apiUrl();
    return !url.isEmpty() && !QUrl(url).host().endsWith(QStringLiteral(".invalid"));
}

void AccountClient::setApiUrl(const QString &url) {
    m_apiUrl = url.trimmed();
    while (m_apiUrl.endsWith(QLatin1Char('/')))
        m_apiUrl.chop(1);
}

void AccountClient::switchApiUrl(const QString &url) {
    QString next = url.trimmed();
    while (next.endsWith(QLatin1Char('/')))
        next.chop(1);
    if (next == m_apiUrl)
        return;

    invalidateAuthentication();
    m_apiUrl = next;
}

void AccountClient::invalidateAuthentication() {
    ++m_requestGeneration;
    for (QNetworkReply *reply : m_net->findChildren<QNetworkReply *>())
        if (reply->isRunning())
            reply->abort();
    m_accessToken.clear();
    m_refreshToken.clear();
    m_credentialDeviceId.clear();
    m_persistRefreshToken = false;
    m_account = {};
}

void AccountClient::setTimeoutMs(int ms) {
    m_timeoutMs = ms;
}

bool AccountClient::isLoggedIn() const {
    return !m_accessToken.isEmpty();
}

AccountInfo AccountClient::account() const {
    return m_account;
}

void AccountClient::forgetStoredSession(const QString &deviceId) {
    if (!deviceId.isEmpty())
        CredentialStore::remove(keyringId(deviceId));
}

void AccountClient::request(Verb verb, const QString &path, const QByteArray &body,
                            bool authenticated, std::function<void(QNetworkReply *)> handler,
                            bool retryAfterRefresh) {
    const quint64 generation = m_requestGeneration;
    const QString root = m_apiUrl.isEmpty() ? apiUrl() : m_apiUrl;
    QNetworkRequest req{QUrl(root + path)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("FileCommander/%1").arg(QStringLiteral(TTC_VERSION)));
    // Lets the server refuse a client whose protocol predates a breaking change
    // (currently: credentials moved out of the URL query) instead of letting it
    // half-work.
    req.setRawHeader("X-FileCommander-Protocol",
                     QByteArray::number(AccountClient::kProtocolVersion));
    if (authenticated)
        req.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());

    QNetworkReply *reply = nullptr;
    switch (verb) {
    case Verb::Get:
        reply = m_net->get(req);
        break;
    case Verb::Post:
        reply = m_net->post(req, body);
        break;
    case Verb::Delete:
        reply = m_net->deleteResource(req);
        break;
    }

    // Same guard UpdateChecker needs: a server that accepts the connection and
    // then goes quiet would otherwise leave the caller waiting forever, with
    // neither signal ever firing.
    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply] {
        if (reply->isRunning())
            reply->abort(); // surfaces as OperationCanceledError below
    });
    timeout->start(m_timeoutMs);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, timeout, verb, path, body, authenticated, handler,
             retryAfterRefresh, generation] {
                timeout->stop();
                timeout->deleteLater();
                reply->deleteLater();
                if (generation != m_requestGeneration)
                    return;

                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                // An expired access token is the ordinary case, not an error:
                // renew it from this session's refresh token and replay the
                // request once. The replay cannot retry again.
                if (authenticated && status == 401 && retryAfterRefresh &&
                    !m_refreshToken.isEmpty()) {
                    const QByteArray refreshBody =
                        QJsonDocument(QJsonObject{{"refresh_token", m_refreshToken}}).toJson();
                    const QString email = m_account.email;
                    request(Verb::Post, QStringLiteral("/v1/auth/refresh"), refreshBody,
                            false,
                            [this, email, verb, path, body, handler](QNetworkReply *r) {
                                if (!acceptTokens(r->readAll(), email)) {
                                    m_accessToken.clear();
                                    m_refreshToken.clear();
                                    emit requestFailed(tr("Session expired, please sign in again."));
                                    return;
                                }
                                request(verb, path, body, true, handler, false);
                            },
                            false);
                    return;
                }
                handler(reply);
            });
}

QString AccountClient::errorText(QNetworkReply *reply, const QByteArray &body) {
    // The server puts a human-readable reason in FastAPI's "detail" field;
    // prefer it over Qt's generic description of the status code.
    const QJsonValue detail = parseObject(body).value(QStringLiteral("detail"));
    if (detail.isString() && !detail.toString().isEmpty())
        return detail.toString();
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return tr("The account server did not respond.");
    if (reply->error() != QNetworkReply::NoError)
        return reply->errorString();
    return tr("The account server returned an unexpected reply.");
}

bool AccountClient::acceptTokens(const QByteArray &body, const QString &email) {
    const QJsonObject o = parseObject(body);
    const QString access = o.value(QStringLiteral("access_token")).toString();
    const QString refresh = o.value(QStringLiteral("refresh_token")).toString();
    const QString deviceId = o.value(QStringLiteral("device_id")).toString();
    if (access.isEmpty() || refresh.isEmpty() || deviceId.isEmpty())
        return false;

    m_accessToken = access;
    m_refreshToken = refresh;
    m_account.email = email;
    m_account.deviceId = deviceId;
    if (!m_credentialDeviceId.isEmpty() && m_credentialDeviceId != deviceId)
        CredentialStore::remove(keyringId(m_credentialDeviceId));
    m_credentialDeviceId = deviceId;
    // Keyring, never the INI. An unchecked login keeps the refresh token only
    // in this object, which is enough to renew the active session without
    // leaving a credential for the next process.
    if (m_persistRefreshToken)
        CredentialStore::save(keyringId(deviceId), refresh);
    else
        CredentialStore::remove(keyringId(deviceId));
    return true;
}

void AccountClient::registerAccount(const QString &email, const QString &password) {
    if (!apiUrlIsConfigured() && m_apiUrl.isEmpty()) {
        emit requestFailed(tr("No account server is configured for this build."));
        return;
    }
    invalidateAuthentication();
    const QByteArray body =
        QJsonDocument(QJsonObject{{"email", email}, {"password", password}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/register"), body, false,
            [this, email](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 201)
                    emit registered(email);
                else if (status == 426)
                    emit updateRequired(errorText(reply, payload));
                else
                    emit requestFailed(errorText(reply, payload));
            });
}

void AccountClient::login(const QString &email, const QString &password,
                          const QString &deviceName, const QString &deviceId,
                          bool rememberSession) {
    if (!apiUrlIsConfigured() && m_apiUrl.isEmpty()) {
        emit requestFailed(tr("No account server is configured for this build."));
        return;
    }
    invalidateAuthentication();
    m_persistRefreshToken = rememberSession;
    m_credentialDeviceId = deviceId;
    if (!rememberSession)
        forgetStoredSession(deviceId);
    const QByteArray body = QJsonDocument(QJsonObject{
                                              {"email", email},
                                              {"password", password},
                                              {"device_name", deviceName},
                                              {"platform",
#ifdef Q_OS_WIN
                                               QStringLiteral("windows")
#else
                                               QStringLiteral("linux")
#endif
                                              },
                                              {"device_id", deviceId},
                                          })
                                .toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/login"), body, false,
            [this, email](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 426)
                    emit updateRequired(errorText(reply, payload));
                else if (acceptTokens(payload, email))
                    emit loggedIn(m_account);
                else
                    emit requestFailed(errorText(reply, payload));
            });
}

void AccountClient::restoreSession(const QString &email, const QString &deviceId) {
    QString stored;
    CredentialStore::load(keyringId(deviceId), &stored);
    if (stored.isEmpty()) {
        emit requestFailed(tr("No saved sign-in for this device."));
        return;
    }
    invalidateAuthentication();
    // Set before the request so a 401 on the retry path knows which session is
    // being restored, and so a rotated token returns to the keyring.
    m_account.email = email;
    m_account.deviceId = deviceId;
    m_credentialDeviceId = deviceId;
    m_refreshToken = stored;
    m_persistRefreshToken = true;

    const QByteArray body =
        QJsonDocument(QJsonObject{{"refresh_token", m_refreshToken}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/refresh"), body, false,
            [this, email](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 426)
                    emit updateRequired(errorText(reply, payload));
                else if (acceptTokens(payload, email))
                    emit loggedIn(m_account);
                else
                    emit requestFailed(errorText(reply, payload));
            });
}

void AccountClient::logout() {
    const QString deviceId = m_account.deviceId;
    const QString credentialDeviceId = m_credentialDeviceId;
    ++m_requestGeneration;
    for (QNetworkReply *reply : m_net->findChildren<QNetworkReply *>())
        if (reply->isRunning())
            reply->abort();
    if (isLoggedIn()) {
        // Best effort: the server-side revoke is the tidy half, but the session
        // is over locally whatever the server says, so the signal does not wait
        // on the reply's outcome.
        request(Verb::Post, QStringLiteral("/v1/auth/logout"), QByteArray(), true,
                [](QNetworkReply *) {}, false);
    }
    m_accessToken.clear();
    m_refreshToken.clear();
    m_credentialDeviceId.clear();
    m_persistRefreshToken = false;
    m_account = {};
    forgetStoredSession(deviceId);
    if (credentialDeviceId != deviceId)
        forgetStoredSession(credentialDeviceId);
    emit loggedOut();
}

void AccountClient::fetchDevices() {
    if (!isLoggedIn()) {
        emit requestFailed(tr("Not signed in."));
        return;
    }
    request(Verb::Get, QStringLiteral("/v1/devices"), QByteArray(), true,
            [this](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const QJsonDocument doc = QJsonDocument::fromJson(payload);
                if (!doc.isArray()) {
                    emit requestFailed(errorText(reply, payload));
                    return;
                }
                QVector<AccountDeviceInfo> devices;
                const QJsonArray array = doc.array();
                devices.reserve(array.size());
                for (const QJsonValue &v : array) {
                    const QJsonObject o = v.toObject();
                    AccountDeviceInfo d;
                    d.id = o.value(QStringLiteral("id")).toString();
                    d.name = o.value(QStringLiteral("name")).toString();
                    d.platform = o.value(QStringLiteral("platform")).toString();
                    d.online = o.value(QStringLiteral("online")).toBool();
                    d.self = o.value(QStringLiteral("self")).toBool();
                    d.lastSeen = o.value(QStringLiteral("last_seen")).toString();
                    for (const QJsonValue &a : o.value(QStringLiteral("lan_addrs")).toArray())
                        d.lanAddresses.append(a.toString());
                    for (const QJsonValue &a : o.value(QStringLiteral("shares")).toArray())
                        d.shares.append(a.toString());
                    if (!d.id.isEmpty())
                        devices.append(d);
                }
                emit devicesReady(devices);
            });
}

QString AccountClient::agentSocketUrl() const {
    if (!isLoggedIn())
        return QString();
    const QString root = m_apiUrl.isEmpty() ? apiUrl() : m_apiUrl;
    QUrl url(root + QStringLiteral("/v1/agent"));
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                         : QStringLiteral("ws"));
    // No credential in the URL: the access token rides as an Authorization
    // header (agentSocketRequest), because a query string is written to access
    // logs and a header is not.
    return url.toString();
}

QNetworkRequest AccountClient::agentSocketRequest() const {
    QNetworkRequest request{QUrl(agentSocketUrl())};
    if (!request.url().isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
        request.setRawHeader("X-FileCommander-Protocol",
                             QByteArray::number(AccountClient::kProtocolVersion));
    }
    return request;
}

QString AccountClient::relaySocketUrl(const QString &sessionId) const {
    const QString root = m_apiUrl.isEmpty() ? apiUrl() : m_apiUrl;
    QUrl url(root + QStringLiteral("/v1/relay/") + sessionId);
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                         : QStringLiteral("ws"));
    // The ticket is the whole authority here, which is what lets the accepting
    // side open a socket for a session it did not start -- but it travels as an
    // Authorization header set by RelayTunnel, never in this URL.
    return url.toString();
}

void AccountClient::removeDevice(const QString &deviceId) {
    if (!isLoggedIn()) {
        emit requestFailed(tr("Not signed in."));
        return;
    }
    request(Verb::Delete, QStringLiteral("/v1/devices/") + deviceId, QByteArray(), true,
            [this, deviceId](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                // 404 counts as removed: the row is gone either way, and telling
                // the user a device they just removed cannot be found is noise.
                if (status != 204 && status != 200 && status != 404) {
                    emit requestFailed(errorText(reply, payload));
                    return;
                }
                emit deviceRemoved(deviceId);
                fetchDevices();
            });
}

void AccountClient::openSession(const QString &deviceId) {
    if (!isLoggedIn()) {
        emit requestFailed(tr("Not signed in."));
        return;
    }
    const QByteArray body =
        QJsonDocument(QJsonObject{{"device_id", deviceId}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/session"), body, true,
            [this](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const QJsonObject o = parseObject(payload);
                AccountSession session;
                session.sessionId = o.value(QStringLiteral("session_id")).toString();
                session.ticket = o.value(QStringLiteral("ticket")).toString();
                session.peerPort = static_cast<quint16>(
                    o.value(QStringLiteral("peer_port")).toInt());
                session.peerPin = o.value(QStringLiteral("peer_pin")).toString();
                session.expiresIn = o.value(QStringLiteral("expires_in")).toInt();
                for (const QJsonValue &a : o.value(QStringLiteral("peer_lan_addrs")).toArray())
                    session.peerLanAddresses.append(a.toString());
                if (session.sessionId.isEmpty() || session.ticket.isEmpty()) {
                    emit requestFailed(errorText(reply, payload));
                    return;
                }
                emit sessionReady(session);
            });
}
