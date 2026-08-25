#include "AccountClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <utility>

#include "CredentialStore.h"
#include "EmailAddress.h"
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

QString targetedClipboardSendPath(const QString &targetDeviceId) {
    return QStringLiteral("/v1/clipboard/send-targeted?target=") +
           QString::fromLatin1(QUrl::toPercentEncoding(targetDeviceId));
}

ClipboardDeliveryInfo parseClipboardDelivery(const QJsonObject &o) {
    ClipboardDeliveryInfo delivery;
    delivery.id = o.value(QStringLiteral("id")).toString();
    delivery.payloadId = o.value(QStringLiteral("payload_id")).toString();
    delivery.kind = o.value(QStringLiteral("kind")).toString();
    delivery.mime = o.value(QStringLiteral("mime")).toString();
    delivery.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
    delivery.width = o.value(QStringLiteral("width")).toInt();
    delivery.height = o.value(QStringLiteral("height")).toInt();
    delivery.sha256 = o.value(QStringLiteral("sha256")).toString();
    delivery.sourceDeviceId = o.value(QStringLiteral("source_device_id")).toString();
    delivery.sourceDeviceName = o.value(QStringLiteral("source_device_name")).toString();
    delivery.created = o.value(QStringLiteral("created")).toString();
    delivery.expires = o.value(QStringLiteral("expires")).toString();
    return delivery;
}

struct ClipboardDownloadState {
    ClipboardDownloadState(const QString &destinationPartPath, qint64 expectedSize,
                           QByteArray expectedSha256)
        : output(destinationPartPath), expectedSize(expectedSize),
          expectedSha256(std::move(expectedSha256)),
          hash(QCryptographicHash::Sha256) {}

    QSaveFile output;
    qint64 expectedSize;
    QByteArray expectedSha256;
    QCryptographicHash hash;
    qint64 received = 0;
    bool headersChecked = false;
    bool failed = false;
};

} // namespace

AccountClient::AccountClient(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {
    qRegisterMetaType<AccountInfo>();
    qRegisterMetaType<QVector<AccountDeviceInfo>>();
    qRegisterMetaType<AccountSession>();
    qRegisterMetaType<ClipboardDeliveryInfo>();
    qRegisterMetaType<QVector<ClipboardDeliveryInfo>>();
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
    ++m_deviceListGeneration;
    for (QNetworkReply *reply : m_net->findChildren<QNetworkReply *>())
        if (reply->isRunning())
            reply->abort();
    m_accessToken.clear();
    m_refreshToken.clear();
    m_credentialDeviceId.clear();
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

QNetworkRequest AccountClient::requestFor(const QString &path, bool authenticated) const {
    const QString root = m_apiUrl.isEmpty() ? apiUrl() : m_apiUrl;
    QNetworkRequest req{QUrl(root + path)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("FileCommander/%1").arg(QStringLiteral(TTC_VERSION)));
    req.setRawHeader("X-FileCommander-Protocol",
                     QByteArray::number(AccountClient::kProtocolVersion));
    if (authenticated)
        req.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    return req;
}

void AccountClient::request(Verb verb, const QString &path, const QByteArray &body,
                            bool authenticated, std::function<void(QNetworkReply *)> handler,
                            bool retryAfterRefresh, QByteArray contentType,
                            std::function<void()> refreshFailureHandler) {
    const quint64 generation = m_requestGeneration;
    QNetworkRequest req = requestFor(path, authenticated);
    if (!contentType.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);

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
            [this, reply, timeout, verb, path, body, authenticated, handler, contentType,
             retryAfterRefresh, refreshFailureHandler, generation] {
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
                    refreshAccessToken([this, verb, path, body, handler, contentType,
                                        refreshFailureHandler](bool refreshed) {
                        if (refreshed)
                            request(verb, path, body, true, handler, false, contentType,
                                    refreshFailureHandler);
                        else if (refreshFailureHandler)
                            refreshFailureHandler();
                    });
                    return;
                }
                handler(reply);
            });
}

void AccountClient::refreshAccessToken(std::function<void(bool)> handler) {
    const QByteArray body =
        QJsonDocument(QJsonObject{{"refresh_token", m_refreshToken}}).toJson();
    const QString email = m_account.email;
    request(Verb::Post, QStringLiteral("/v1/auth/refresh"), body, false,
            [this, email, handler](QNetworkReply *reply) {
                if (!acceptTokens(reply->readAll(), email)) {
                    m_accessToken.clear();
                    m_refreshToken.clear();
                    emit requestFailed(tr("Session expired, please sign in again."));
                    handler(false);
                    return;
                }
                handler(true);
            },
            false);
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
    // Keyring, never the INI: a refresh token is a password by another name.
    // A keyring that refuses the write costs the user a re-login next launch,
    // which is worth more than writing the token to a plain file.
    CredentialStore::save(keyringId(deviceId), refresh);
    return true;
}

void AccountClient::registerAccount(const QString &email, const QString &password) {
    const std::optional<QString> canonical = AccountEmail::canonicalize(email);
    if (!canonical) {
        emit requestFailed(tr("Enter a valid email address."));
        return;
    }
    if (!apiUrlIsConfigured() && m_apiUrl.isEmpty()) {
        emit requestFailed(tr("No account server is configured for this build."));
        return;
    }
    invalidateAuthentication();
    const QByteArray body = QJsonDocument(QJsonObject{{"email", *canonical}, {"password", password}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/register"), body, false,
            [this, email = *canonical](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 201)
                    emit registered(email);
                else if (status == 426)
                    emit updateRequired(errorText(reply, payload));
                else
                    emit requestFailed(errorText(reply, payload));
            });
}

void AccountClient::login(const QString &email, const QString &password,
                          const QString &deviceName, const QString &deviceId) {
    const std::optional<QString> canonical = AccountEmail::canonicalize(email);
    if (!canonical) {
        emit requestFailed(tr("Enter a valid email address."));
        return;
    }
    if (!apiUrlIsConfigured() && m_apiUrl.isEmpty()) {
        emit requestFailed(tr("No account server is configured for this build."));
        return;
    }
    invalidateAuthentication();
    m_credentialDeviceId = deviceId;
    const QByteArray body = QJsonDocument(QJsonObject{
                                              {"email", *canonical},
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
            [this, email = *canonical](QNetworkReply *reply) {
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
    const std::optional<QString> canonical = AccountEmail::canonicalize(email);
    if (!canonical) {
        emit requestFailed(tr("Enter a valid email address."));
        return;
    }
    QString stored;
    CredentialStore::load(keyringId(deviceId), &stored);
    if (stored.isEmpty()) {
        emit requestFailed(tr("No saved sign-in for this device."));
        return;
    }
    invalidateAuthentication();
    // Set before the request so a 401 on the retry path knows which session is
    // being restored, and so a rotated token returns to the keyring.
    m_account.email = *canonical;
    m_account.deviceId = deviceId;
    m_credentialDeviceId = deviceId;
    m_refreshToken = stored;

    const QByteArray body = QJsonDocument(QJsonObject{{"refresh_token", m_refreshToken}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/auth/refresh"), body, false,
            [this, email = *canonical](QNetworkReply *reply) {
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
    const quint64 generation = ++m_deviceListGeneration;
    request(Verb::Get, QStringLiteral("/v1/devices"), QByteArray(), true,
            [this, generation](QNetworkReply *reply) {
                if (generation != m_deviceListGeneration)
                    return;
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
    openSessionRequest(deviceId);
}

void AccountClient::openSessionRequest(const QString &deviceId) {
    if (!isLoggedIn()) {
        emit requestFailed(tr("Not signed in."));
        return;
    }
    const QByteArray body = QJsonDocument(QJsonObject{{"device_id", deviceId}}).toJson();
    request(Verb::Post, QStringLiteral("/v1/session"), body, true,
            [this, deviceId](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const QJsonObject o = parseObject(payload);
                AccountSession session;
                session.sessionId = o.value(QStringLiteral("session_id")).toString();
                session.ticket = o.value(QStringLiteral("ticket")).toString();
                session.peerPort = static_cast<quint16>(o.value(QStringLiteral("peer_port")).toInt());
                session.peerPin = o.value(QStringLiteral("peer_pin")).toString();
                session.expiresIn = o.value(QStringLiteral("expires_in")).toInt();
                for (const QJsonValue &a : o.value(QStringLiteral("peer_lan_addrs")).toArray())
                    session.peerLanAddresses.append(a.toString());
                if (session.sessionId.isEmpty() || session.ticket.isEmpty()) {
                    emit requestFailed(errorText(reply, payload));
                    return;
                }
                emit sessionReady(session);
                emit sessionReadyForDevice(deviceId, session);
            });
}

void AccountClient::finishClipboardSend(QNetworkReply *reply) {
    const QByteArray payload = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
        const QString error = errorText(reply, payload);
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    const QJsonObject result = parseObject(payload);
    const QString payloadId = result.value(QStringLiteral("payload_id")).toString();
    if (payloadId.isEmpty()) {
        const QString error = errorText(reply, payload);
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    emit clipboardSendFinished(payloadId, result.value(QStringLiteral("recipient_count")).toInt());
}

void AccountClient::sendClipboardText(const QString &text) {
    if (!isLoggedIn()) {
        const QString error = tr("Not signed in.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    request(Verb::Post, QStringLiteral("/v1/clipboard/send"), text.toUtf8(), true,
            [this](QNetworkReply *reply) { finishClipboardSend(reply); }, true,
            QByteArrayLiteral("text/plain; charset=utf-8"), [this] {
                emit clipboardSendFailed(tr("Session expired, please sign in again."));
            });
}

void AccountClient::sendClipboardTextToTarget(const QString &text, const QString &targetDeviceId) {
    if (!isLoggedIn()) {
        const QString error = tr("Not signed in.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    if (targetDeviceId.isEmpty()) {
        const QString error = tr("No clipboard target device was selected.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    request(Verb::Post, targetedClipboardSendPath(targetDeviceId), text.toUtf8(), true,
            [this](QNetworkReply *reply) { finishClipboardSend(reply); }, true,
            QByteArrayLiteral("text/plain; charset=utf-8"), [this] {
                emit clipboardSendFailed(tr("Session expired, please sign in again."));
            });
}

void AccountClient::sendClipboardImageFile(const QString &filePath, const QString &mime,
                                           int width, int height, const QByteArray &sha256) {
    if (!isLoggedIn()) {
        const QString error = tr("Not signed in.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    sendClipboardImageFileRequest(filePath, mime, width, height, sha256,
                                  QStringLiteral("/v1/clipboard/send"), true);
}

void AccountClient::sendClipboardImageFileToTarget(const QString &filePath, const QString &mime,
                                                   int width, int height, const QByteArray &sha256,
                                                   const QString &targetDeviceId) {
    if (!isLoggedIn()) {
        const QString error = tr("Not signed in.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    if (targetDeviceId.isEmpty()) {
        const QString error = tr("No clipboard target device was selected.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        return;
    }
    sendClipboardImageFileRequest(filePath, mime, width, height, sha256,
                                  targetedClipboardSendPath(targetDeviceId), true);
}

void AccountClient::sendClipboardImageFileRequest(const QString &filePath, const QString &mime,
                                                  int width, int height, const QByteArray &sha256,
                                                  const QString &path, bool retryAfterRefresh) {
    auto *file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        const QString error = tr("Could not open the clipboard image.");
        emit requestFailed(error);
        emit clipboardSendFailed(error);
        delete file;
        return;
    }

    QByteArray hash = sha256;
    if (hash.size() == QCryptographicHash::hashLength(QCryptographicHash::Sha256))
        hash = hash.toHex();
    QNetworkRequest request = requestFor(path, true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, mime);
    request.setRawHeader("X-Clipboard-Width", QByteArray::number(width));
    request.setRawHeader("X-Clipboard-Height", QByteArray::number(height));
    request.setRawHeader("X-Clipboard-Sha256", hash.toLower());

    const quint64 generation = m_requestGeneration;
    QNetworkReply *reply = m_net->post(request, file);
    file->setParent(reply);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    timeout->start(m_timeoutMs);
    connect(reply, &QNetworkReply::uploadProgress, this,
            [this](qint64 sent, qint64 total) {
                if (total > 0)
                    emit clipboardSendProgress(sent, total);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, timeout, generation, filePath, mime, width, height, sha256, path,
             retryAfterRefresh] {
                timeout->stop();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (generation == m_requestGeneration && status == 401 && retryAfterRefresh &&
                    !m_refreshToken.isEmpty()) {
                    refreshAccessToken([this, filePath, mime, width, height, sha256, path, generation](bool refreshed) {
                        if (refreshed && generation == m_requestGeneration)
                            sendClipboardImageFileRequest(filePath, mime, width, height, sha256, path, false);
                        else if (generation == m_requestGeneration)
                            emit clipboardSendFailed(tr("Session expired, please sign in again."));
                    });
                } else if (generation == m_requestGeneration) {
                    finishClipboardSend(reply);
                }
                reply->deleteLater();
            });
}

void AccountClient::fetchClipboardDeliveries() {
    if (!isLoggedIn()) {
        emit requestFailed(tr("Not signed in."));
        return;
    }
    request(Verb::Get, QStringLiteral("/v1/clipboard/deliveries"), QByteArray(), true,
            [this](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const QJsonObject result = parseObject(payload);
                if (!result.contains(QStringLiteral("deliveries"))) {
                    emit requestFailed(errorText(reply, payload));
                    return;
                }
                QVector<ClipboardDeliveryInfo> deliveries;
                const QJsonArray listed = result.value(QStringLiteral("deliveries")).toArray();
                deliveries.reserve(listed.size());
                for (const QJsonValue &value : listed) {
                    const ClipboardDeliveryInfo delivery = parseClipboardDelivery(value.toObject());
                    if (!delivery.id.isEmpty())
                        deliveries.append(delivery);
                }
                emit clipboardDeliveriesReady(deliveries);
            });
}

void AccountClient::downloadClipboardDelivery(const ClipboardDeliveryInfo &delivery,
                                              const QString &destinationPartPath) {
    if (!isLoggedIn()) {
        const QString error = tr("Not signed in.");
        emit requestFailed(error);
        emit clipboardDeliveryDownloadFailed(delivery.id, error);
        return;
    }
    const QByteArray expectedSha256 = delivery.sha256.toLatin1().trimmed().toLower();
    if (delivery.id.isEmpty() || destinationPartPath.isEmpty() || delivery.size < 0 ||
        expectedSha256.size() != 64) {
        const QString error = tr("Invalid clipboard delivery.");
        emit requestFailed(error);
        emit clipboardDeliveryDownloadFailed(delivery.id, error);
        return;
    }
    downloadClipboardDeliveryRequest(delivery, destinationPartPath, true);
}

void AccountClient::downloadClipboardDeliveryRequest(const ClipboardDeliveryInfo &delivery,
                                                     const QString &destinationPartPath,
                                                     bool retryAfterRefresh) {
    const QByteArray expectedSha256 = delivery.sha256.toLatin1().trimmed().toLower();
    auto state = std::make_shared<ClipboardDownloadState>(destinationPartPath, delivery.size,
                                                          expectedSha256);
    if (!state->output.open(QIODevice::WriteOnly)) {
        const QString error = tr("Could not create the clipboard download file.");
        emit requestFailed(error);
        emit clipboardDeliveryDownloadFailed(delivery.id, error);
        return;
    }

    const quint64 generation = m_requestGeneration;
    QNetworkReply *reply = m_net->get(requestFor(
        QStringLiteral("/v1/clipboard/deliveries/") + delivery.id + QStringLiteral("/content"), true));
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    timeout->start(m_timeoutMs);

    const auto fail = [this, state, delivery](const QString &reason) {
        if (state->failed)
            return;
        state->failed = true;
        state->output.cancelWriting();
        emit requestFailed(reason);
        emit clipboardDeliveryDownloadFailed(delivery.id, reason);
    };
    const auto checkHeaders = [reply, state, fail]() {
        if (state->headersChecked)
            return !state->failed;
        state->headersChecked = true;
        bool lengthOk = false;
        const qint64 contentLength =
            reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&lengthOk);
        const QByteArray contentSha256 = reply->rawHeader("X-Content-Sha256").trimmed().toLower();
        if (!lengthOk || contentLength != state->expectedSize ||
            contentSha256 != state->expectedSha256) {
            fail(QObject::tr("Clipboard delivery integrity validation failed."));
            return false;
        }
        return true;
    };
    const auto consume = [this, reply, state, delivery, checkHeaders, fail] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 || status < 200)
            return;
        if (!checkHeaders()) {
            if (reply->isRunning())
                reply->abort();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        if (state->received > state->expectedSize - chunk.size() ||
            state->output.write(chunk) != chunk.size()) {
            fail(tr("Could not save the clipboard download."));
            if (reply->isRunning())
                reply->abort();
            return;
        }
        state->hash.addData(chunk);
        state->received += chunk.size();
        emit clipboardDownloadProgress(delivery.id, state->received, state->expectedSize);
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, timeout, state, delivery, destinationPartPath, generation, consume,
             checkHeaders, fail, retryAfterRefresh] {
                timeout->stop();
                consume();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (generation != m_requestGeneration) {
                    state->output.cancelWriting();
                } else if (status == 401 && retryAfterRefresh && !m_refreshToken.isEmpty()) {
                    state->output.cancelWriting();
                    refreshAccessToken([this, delivery, destinationPartPath, generation](bool refreshed) {
                        if (refreshed && generation == m_requestGeneration)
                            downloadClipboardDeliveryRequest(delivery, destinationPartPath, false);
                        else if (generation == m_requestGeneration)
                            emit clipboardDeliveryDownloadFailed(
                                delivery.id, tr("Session expired, please sign in again."));
                    });
                } else if (!state->failed) {
                    const QByteArray payload = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
                        fail(errorText(reply, payload));
                    else if (!checkHeaders() || state->received != state->expectedSize ||
                             state->hash.result().toHex() != state->expectedSha256)
                        fail(tr("Clipboard delivery integrity validation failed."));
                    else if (!state->output.commit())
                        fail(tr("Could not save the clipboard download."));
                    else
                        emit clipboardDownloadFinished(delivery.id, destinationPartPath);
                }
                reply->deleteLater();
            });
}

void AccountClient::acknowledgeClipboardDelivery(const QString &deliveryId) {
    if (!isLoggedIn()) {
        const QString error = tr("Not signed in.");
        emit requestFailed(error);
        emit clipboardDeliveryAcknowledgementFailed(deliveryId, error);
        return;
    }
    request(Verb::Post,
            QStringLiteral("/v1/clipboard/deliveries/") + deliveryId + QStringLiteral("/ack"),
            QByteArray(), true, [this, deliveryId](QNetworkReply *reply) {
                const QByteArray payload = reply->readAll();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                    const QString error = errorText(reply, payload);
                    emit requestFailed(error);
                    emit clipboardDeliveryAcknowledgementFailed(deliveryId, error);
                    return;
                }
                emit clipboardDeliveryAcknowledged(deliveryId);
            }, true, QByteArrayLiteral("application/json"), [this, deliveryId] {
                emit clipboardDeliveryAcknowledgementFailed(
                    deliveryId, tr("Session expired, please sign in again."));
            });
}
