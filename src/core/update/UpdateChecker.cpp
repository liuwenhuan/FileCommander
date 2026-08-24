#include "UpdateChecker.h"

#include <QDate>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include "version.h"

namespace {

constexpr qint64 kMaximumManifestBytes = 64 * 1024;
const QString kOfficialManifestUrl = QStringLiteral("https://fc.aigutta.com/version.json");
const QString kUpdatePageUrl = QStringLiteral("https://fc.aigutta.com/update.html");

QVector<int> versionParts(const QString &raw) {
    const QStringList tokens = raw.split(QLatin1Char('.'));
    QVector<int> parts;
    for (const QString &token : tokens) {
        bool ok = false;
        const int value = token.toInt(&ok);
        if (!ok || value < 0)
            return {};
        parts.append(value);
    }
    return parts;
}

bool looksLikeVersion(const QString &raw) {
    static const QRegularExpression version(QStringLiteral("\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z"));
    if (!version.match(raw).hasMatch())
        return false;
    for (const QString &component : raw.split(QLatin1Char('.'))) {
        bool ok = false;
        component.toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

bool isOfficialManifestUrl(const QString &raw) {
    const QUrl url(raw);
    return url.isValid() && url.scheme() == QLatin1String("https") &&
           url.host().compare(QStringLiteral("fc.aigutta.com"), Qt::CaseInsensitive) == 0 &&
           url.path() == QLatin1String("/version.json") && !url.hasQuery() && !url.hasFragment();
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString UpdateChecker::manifestUrl() { return kOfficialManifestUrl; }
bool UpdateChecker::manifestUrlIsConfigured() { return true; }
QString UpdateChecker::updatePageUrl() { return kUpdatePageUrl; }

void UpdateChecker::setManifestUrl(const QString &url) { m_manifestUrl = url; }
void UpdateChecker::setTimeoutMs(int ms) { m_timeoutMs = ms; }

bool UpdateChecker::isNewer(const QString &remote, const QString &local) {
    if (!looksLikeVersion(remote) || !looksLikeVersion(local))
        return false;
    const QVector<int> r = versionParts(remote);
    const QVector<int> l = versionParts(local);
    const int n = qMax(r.size(), l.size());
    for (int i = 0; i < n; ++i) {
        const int rv = i < r.size() ? r.at(i) : 0;
        const int lv = i < l.size() ? l.at(i) : 0;
        if (rv != lv)
            return rv > lv;
    }
    return false;
}

UpdateChecker::ParseResult UpdateChecker::parseManifest(const QByteArray &body,
                                                        const QString &localVersion,
                                                        const QString &segmentKey,
                                                        UpdateInfo *info, QString *error) {
    Q_UNUSED(segmentKey);
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return ParseResult::Invalid;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QObject::tr("Malformed update manifest: %1").arg(parseError.errorString()));

    const QJsonObject root = doc.object();
    const QJsonValue schema = root.value(QStringLiteral("schema"));
    if (!schema.isDouble() || schema.toDouble() != 2.0)
        return fail(QObject::tr("Update manifest has an unsupported schema."));
    const QString remoteVersion = root.value(QStringLiteral("version")).toString().trimmed();
    const QString date = root.value(QStringLiteral("date")).toString();
    const QString notes = root.value(QStringLiteral("notes")).toString();
    if (!looksLikeVersion(remoteVersion))
        return fail(QObject::tr("Update manifest has an unreadable version (\"%1\").").arg(remoteVersion));
    const QDate parsedDate = QDate::fromString(date, Qt::ISODate);
    if (!parsedDate.isValid() || parsedDate.toString(Qt::ISODate) != date)
        return fail(QObject::tr("Update manifest has an unreadable release date."));
    if (!root.value(QStringLiteral("notes")).isString())
        return fail(QObject::tr("Update manifest is missing release notes."));
    if (!isNewer(remoteVersion, localVersion))
        return ParseResult::UpToDate;
    if (info) {
        info->version = remoteVersion;
        info->date = date;
        info->notes = notes;
    }
    return ParseResult::UpdateAvailable;
}

void UpdateChecker::checkForUpdates() {
    if (m_reply) {
        emit checkFailed(tr("An update check is already in progress."));
        return;
    }
    const QString urlText = m_manifestUrl.isEmpty() ? manifestUrl() : m_manifestUrl;
    if (m_manifestUrl.isEmpty() && !isOfficialManifestUrl(urlText)) {
        emit checkFailed(tr("The configured update manifest URL is not usable: %1").arg(urlText));
        return;
    }
    const QUrl url(urlText);
    if (!url.isValid() || url.host().isEmpty() ||
        (m_manifestUrl.isEmpty() && url.scheme() != QLatin1String("https"))) {
        emit checkFailed(tr("The configured update manifest URL is not usable: %1").arg(urlText));
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("FileCommander/%1").arg(QStringLiteral(TTC_VERSION)));
    m_manifestBody.clear();
    m_manifestBytes = 0;
    m_tooLarge = false;
    m_reply = m_net->get(request);
    m_reply->setReadBufferSize(kMaximumManifestBytes + 1);
    const QVariant contentLength = m_reply->header(QNetworkRequest::ContentLengthHeader);
    if (contentLength.isValid() && contentLength.toLongLong() > kMaximumManifestBytes) {
        m_tooLarge = true;
        m_reply->abort();
    }
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_reply)
            return;
        const QByteArray chunk = m_reply->readAll();
        m_manifestBytes += chunk.size();
        if (m_manifestBytes > kMaximumManifestBytes) {
            m_tooLarge = true;
            m_reply->abort();
            return;
        }
        m_manifestBody.append(chunk);
    });
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, m_reply, [reply = m_reply] {
        if (reply->isRunning())
            reply->abort();
    });
    m_timeout->start(m_timeoutMs);
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply] { onManifestFinished(reply); });
}

void UpdateChecker::onManifestFinished(QNetworkReply *reply) {
    if (reply != m_reply)
        return;
    const QByteArray tail = reply->readAll();
    m_manifestBytes += tail.size();
    if (m_manifestBytes <= kMaximumManifestBytes)
        m_manifestBody.append(tail);
    else
        m_tooLarge = true;
    const bool timedOut = m_timeout && !m_timeout->isActive();
    if (m_timeout) {
        m_timeout->stop();
        m_timeout->deleteLater();
        m_timeout = nullptr;
    }
    m_reply = nullptr;
    reply->deleteLater();
    if (m_tooLarge) {
        emit checkFailed(tr("The update manifest is too large."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (timedOut && reply->error() == QNetworkReply::OperationCanceledError)
            emit checkFailed(tr("The update server did not respond in time."));
        else
            emit checkFailed(reply->errorString());
        return;
    }
    UpdateInfo info;
    QString error;
    switch (parseManifest(m_manifestBody, QStringLiteral(TTC_VERSION), QString(), &info, &error)) {
    case ParseResult::UpdateAvailable: emit updateAvailable(info); return;
    case ParseResult::UpToDate: emit noUpdate(); return;
    case ParseResult::Invalid: emit checkFailed(error); return;
    }
}
