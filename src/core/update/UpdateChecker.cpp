#include "UpdateChecker.h"

#include "Updater.h"

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

// The build-time default (docs/UPDATE_SERVER.md section 5). `.invalid` is the
// reserved never-resolves TLD from RFC 2606, so an unconfigured build cannot
// reach anybody's server even by accident.
const QString kPlaceholderHostSuffix = QStringLiteral(".invalid");

const char kUrlEnvVar[] = "FILECOMMANDER_UPDATE_MANIFEST_URL";

// Split a dotted version into its numeric components, dropping a leading 'v' and
// any pre-release suffix after the first '-' ("v1.2.3-beta" -> {1, 2, 3}).
QVector<int> versionParts(const QString &raw) {
    QString v = raw.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V')))
        v = v.mid(1);
    const int dash = v.indexOf(QLatin1Char('-'));
    if (dash >= 0)
        v = v.left(dash);

    QVector<int> parts;
    const QStringList tokens = v.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString &token : tokens)
        parts.append(token.toInt());
    return parts;
}

// Whether a manifest's version string is one we can actually compare. It has to
// be checked separately from parsing it: toInt() turns anything unparseable into
// 0, so "latest" would compare as 0.0.0 and quietly read as "you are up to
// date" -- a broken manifest that hides every release instead of reporting
// itself.
bool looksLikeVersion(const QString &raw) {
    QString v = raw.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V')))
        v = v.mid(1);
    const int dash = v.indexOf(QLatin1Char('-'));
    if (dash >= 0)
        v = v.left(dash);

    const QStringList tokens = v.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return false;
    for (const QString &token : tokens) {
        bool ok = false;
        const int value = token.toInt(&ok);
        if (!ok || value < 0)
            return false;
    }
    return true;
}

// A package URL has to be one we are willing to hand to QNetworkAccessManager
// and then execute the result of. Anything but http/https (file://, ftp://, a
// relative path) is refused outright rather than "tried and failed": a manifest
// is remote input, and the only reason it would name another scheme is to get
// us to install something from somewhere we did not intend.
bool isAcceptableDownloadUrl(const QString &raw) {
    const QUrl url(raw);
    if (!url.isValid() || url.host().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

bool isSha256Hex(const QString &raw) {
    static const QRegularExpression re(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    return re.match(raw.trimmed()).hasMatch();
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString UpdateChecker::manifestUrl() {
    const QByteArray override = qgetenv(kUrlEnvVar);
    if (!override.isEmpty())
        return QString::fromUtf8(override).trimmed();
    return QStringLiteral(TTC_UPDATE_MANIFEST_URL);
}

bool UpdateChecker::manifestUrlIsConfigured() {
    const QUrl url(manifestUrl());
    return url.isValid() && !url.host().isEmpty()
           && !url.host().endsWith(kPlaceholderHostSuffix, Qt::CaseInsensitive);
}

void UpdateChecker::setManifestUrl(const QString &url) {
    m_manifestUrl = url;
}

void UpdateChecker::setTimeoutMs(int ms) {
    m_timeoutMs = ms;
}

QString UpdateChecker::packageSegmentKey() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#else
    // One Linux binary can be running either way, so this is a runtime question:
    // an AppImage replaces itself, a system install goes through the .deb.
    return Updater::runningAsAppImage() ? QStringLiteral("appimage") : QStringLiteral("deb");
#endif
}

bool UpdateChecker::isNewer(const QString &remote, const QString &local) {
    const QVector<int> r = versionParts(remote);
    const QVector<int> l = versionParts(local);
    const int n = qMax(r.size(), l.size());
    for (int i = 0; i < n; ++i) {
        const int rv = i < r.size() ? r.at(i) : 0;
        const int lv = i < l.size() ? l.at(i) : 0;
        if (rv != lv)
            return rv > lv;
    }
    return false; // equal
}

UpdateChecker::ParseResult UpdateChecker::parseManifest(const QByteArray &body,
                                                        const QString &localVersion,
                                                        const QString &segmentKey,
                                                        UpdateInfo *info, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return ParseResult::Invalid;
    };

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(tr("Malformed update manifest: %1").arg(parseError.errorString()));

    const QJsonObject root = doc.object();
    const QString remoteVersion = root.value(QStringLiteral("version")).toString().trimmed();
    if (remoteVersion.isEmpty())
        return fail(tr("Update manifest is missing a version field."));
    if (!looksLikeVersion(remoteVersion))
        return fail(tr("Update manifest has an unreadable version (\"%1\").").arg(remoteVersion));

    if (!isNewer(remoteVersion, localVersion))
        return ParseResult::UpToDate;

    const QJsonObject segment = root.value(segmentKey).toObject();
    const QString url = segment.value(QStringLiteral("url")).toString().trimmed();
    const QString sha256 = segment.value(QStringLiteral("sha256")).toString().trimmed();
    if (url.isEmpty() || sha256.isEmpty())
        return fail(
            tr("Update manifest has no %1 package for this installation.").arg(segmentKey));
    // Both checks below reject the manifest rather than letting a bad value
    // reach the downloader: a non-http URL is a redirect to somewhere we never
    // meant to fetch from, and a malformed hash is a verification step that
    // could never pass -- better to say so now than after a long download.
    if (!isAcceptableDownloadUrl(url))
        return fail(tr("Update manifest gives an unusable download URL for %1.").arg(segmentKey));
    if (!isSha256Hex(sha256))
        return fail(tr("Update manifest gives a malformed SHA-256 for %1.").arg(segmentKey));

    if (info) {
        info->version = remoteVersion;
        info->date = root.value(QStringLiteral("date")).toString();
        info->notes = root.value(QStringLiteral("notes")).toString();
        info->url = url;
        info->sha256 = sha256;
    }
    return ParseResult::UpdateAvailable;
}

void UpdateChecker::checkForUpdates() {
    const QString url = m_manifestUrl.isEmpty() ? manifestUrl() : m_manifestUrl;
    if (m_manifestUrl.isEmpty() && !manifestUrlIsConfigured()) {
        emit checkFailed(tr("No update server is configured for this build."));
        return;
    }
    if (!isAcceptableDownloadUrl(url)) {
        emit checkFailed(tr("The configured update manifest URL is not usable: %1").arg(url));
        return;
    }

    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // A cached manifest is a manifest that can hide a release. Ask every hop in
    // the path for the live copy; the packages themselves stay cacheable.
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("FileCommander/%1").arg(QStringLiteral(TTC_VERSION)));

    QNetworkReply *reply = m_net->get(request);

    // Abort a server that connects and then goes quiet. Without this neither
    // signal ever fires and the daily check leaks one checker per launch.
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, reply, [reply] {
        if (reply->isRunning())
            reply->abort(); // surfaces as OperationCanceledError below
    });
    m_timeout->start(m_timeoutMs);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { onManifestFinished(reply); });
}

void UpdateChecker::onManifestFinished(QNetworkReply *reply) {
    reply->deleteLater();
    const bool timedOut = m_timeout && !m_timeout->isActive();
    if (m_timeout) {
        m_timeout->stop();
        m_timeout->deleteLater();
        m_timeout = nullptr;
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
    switch (parseManifest(reply->readAll(), QStringLiteral(TTC_VERSION), packageSegmentKey(),
                          &info, &error)) {
    case ParseResult::UpdateAvailable:
        emit updateAvailable(info);
        return;
    case ParseResult::UpToDate:
        emit noUpdate();
        return;
    case ParseResult::Invalid:
        emit checkFailed(error);
        return;
    }
}
