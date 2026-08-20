#include "AccountClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

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
    m_apiUrl = url;
    while (m_apiUrl.endsWith(QLatin1Char('/')))
        m_apiUrl.chop(1);
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
    const QString root = m_apiUrl.isEmpty() ? apiUrl() : m_apiUrl;
    QNetworkRequest req{QUrl(root + path)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("FileCommander/%1").arg(QStringLiteral(TTC_VERSION)));
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
             retryAfterRefresh] {
                timeout->stop();
                timeout->deleteLater();
                reply->deleteLater();

                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                // An expired access token is the ordinary case, not an error:
                // renew it from the keyring refresh token and replay the
                // request once. The replay cannot retry again.
                if (authenticated && status == 401 && retryAfterRefresh &&
                    !m_account.deviceId.isEmpty()) {
                    QString stored;
                    CredentialStore::load(keyringId(m_account.deviceId), &stored);
                    if (!stored.isEmpty()) {
                        const QByteArray refreshBody =
                            QJsonDocument(QJsonObject{{"refresh_token", stored}}).toJson();
                        const QString email = m_account.email;
                        request(Verb::Post, QStringLiteral("/v1/auth/refresh"), refreshBody,
                                false,
                                [this, email, verb, path, body, handler](QNetworkReply *r) {
                                    if (!acceptTokens(r->readAll(), email)) {
                                        m_accessToken.clear();
                                        emit requestFailed(tr("Session expired, please sign in again."));
                                        return;
                                    }
                                    request(verb, path, body, true, handler, false);
                                },
                                false);
                        return;
                    }
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
    m_account.email = email;
    m_account.deviceId = deviceId;
    // Keyring, never the INI: a refresh token is a password by another name.
    // A keyring that refuses the write costs the user a re-login next launch,
    // which is worth strictly more than writing the token to a plain file.
    CredentialStore::save(keyringId(deviceId), refresh);
    return true;
}

void AccountClient::registerAccount(const QString &email, const QString &password) {
    if (!apiUrlIsConfigured() && m_apiUrl.isEmpty()) {
        emit requestFailed(tr("No account server is configured for this build."));
        return;
    }
    const QByteArray body =
        QJsonDocument(QJsonObject{{"email", email}, {"password", password}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/register"), body, false,
            [this, email](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 201)
                    emit registered(email);
                else
                    emit requestFailed(errorText(reply, payload));
            });
}

void AccountClient::login(const QString &email, const QString &password,
                          const QString &deviceName, const QString &deviceId) {
    if (!apiUrlIsConfigured() && m_apiUrl.isEmpty()) {
        emit requestFailed(tr("No account server is configured for this build."));
        return;
    }
    const QByteArray body = QJsonDocument(QJsonObject{
                                              {"email", email},
                                              {"password", password},
                                              {"device_name", deviceName},
                                              {"platform", QStringLiteral(
#ifdef Q_OS_WIN
                                                   "windows"
#else
                                                   "linux"
#endif
                                                   )},
                                              {"device_id", deviceId},
                                          })
                                .toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/login"), body, false,
            [this, email](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                if (acceptTokens(payload, email))
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
    // Set before the request so a 401 on the retry path knows which keyring
    // entry to reach for.
    m_account.email = email;
    m_account.deviceId = deviceId;

    const QByteArray body =
        QJsonDocument(QJsonObject{{"refresh_token", stored}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/refresh"), body, false,
            [this, email](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                if (acceptTokens(payload, email))
                    emit loggedIn(m_account);
                else
                    emit requestFailed(errorText(reply, payload));
            });
}

void AccountClient::logout() {
    const QString deviceId = m_account.deviceId;
    if (isLoggedIn()) {
        // Best effort: the server-side revoke is the tidy half, but the session
        // is over locally whatever the server says, so the signal does not wait
        // on the reply's outcome.
        request(Verb::Post, QStringLiteral("/v1/auth/logout"), QByteArray(), true,
                [](QNetworkReply *) {}, false);
    }
    m_accessToken.clear();
    m_account = {};
    forgetStoredSession(deviceId);
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
    // The token goes in the query rather than a header: QWebSocket can carry a
    // header, but a reconnect built from a stored URL cannot, and the server
    // accepts either.
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), m_accessToken);
    url.setQuery(query);
    return url.toString();
}

QString AccountClient::relaySocketUrl(const QString &sessionId, const QString &ticket) const {
    const QString root = m_apiUrl.isEmpty() ? apiUrl() : m_apiUrl;
    QUrl url(root + QStringLiteral("/v1/relay/") + sessionId);
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                         : QStringLiteral("ws"));
    // No access token: the ticket is the whole authority here, which is what
    // lets the accepting side open a socket for a session it did not start.
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ticket"), ticket);
    url.setQuery(query);
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
