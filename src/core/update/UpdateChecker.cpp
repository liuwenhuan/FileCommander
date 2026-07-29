#include "UpdateChecker.h"

#include "Updater.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

#include "version.h"

namespace {

// URL of the release manifest. Point this at your own static HTTP server before
// building — see docs/UPDATE_SERVER.md for the manifest schema and a deployment
// walkthrough. The placeholder host below is intentionally unreachable.
const QString kUpdateManifestUrl =
    QStringLiteral("https://YOUR_SERVER/FileCommander/version.json");

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

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

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

void UpdateChecker::checkForUpdates() {
    QNetworkRequest request{QUrl(kUpdateManifestUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { onManifestFinished(reply); });
}

void UpdateChecker::onManifestFinished(QNetworkReply *reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit checkFailed(reply->errorString());
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit checkFailed(tr("Malformed update manifest: %1").arg(parseError.errorString()));
        return;
    }

    const QJsonObject root = doc.object();
    const QString remoteVersion = root.value(QStringLiteral("version")).toString();
    if (remoteVersion.isEmpty()) {
        emit checkFailed(tr("Update manifest is missing a version field."));
        return;
    }

    if (!isNewer(remoteVersion, QStringLiteral(TTC_VERSION))) {
        emit noUpdate();
        return;
    }

    // Pick the package segment matching how this build is installed. An AppImage
    // updates itself in place; a system install uses the .deb.
#ifdef Q_OS_WIN
    const QString segmentKey = QStringLiteral("windows");
#else
    const QString segmentKey = Updater::runningAsAppImage() ? QStringLiteral("appimage")
                                                            : QStringLiteral("deb");
#endif
    const QJsonObject segment = root.value(segmentKey).toObject();
    const QString url = segment.value(QStringLiteral("url")).toString();
    const QString sha256 = segment.value(QStringLiteral("sha256")).toString();
    if (url.isEmpty() || sha256.isEmpty()) {
        emit checkFailed(
            tr("Update manifest has no %1 package for this installation.").arg(segmentKey));
        return;
    }

    UpdateInfo info;
    info.version = remoteVersion;
    info.date = root.value(QStringLiteral("date")).toString();
    info.notes = root.value(QStringLiteral("notes")).toString();
    info.url = url;
    info.sha256 = sha256;
    emit updateAvailable(info);
}
