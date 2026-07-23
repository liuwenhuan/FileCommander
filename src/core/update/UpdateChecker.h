#pragma once

#include <QObject>
#include <QString>

// One release as advertised by the server manifest (version.json), already
// resolved to the package matching this install's form (AppImage or deb). See
// UpdateChecker::checkForUpdates for the manifest schema.
struct UpdateInfo {
    QString version; // e.g. "0.2.0"
    QString notes;   // human-readable release notes / changelog
    QString date;    // release date, e.g. "2026-07-20"
    QString url;     // download URL for the matching package
    QString sha256;  // expected SHA-256 of the package (hex, case-insensitive)
};

class QNetworkAccessManager;
class QNetworkReply;

// Fetches the server manifest and reports whether a newer release exists.
// Network I/O only; the actual download/install lives in Updater.
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // GETs the version.json manifest, parses it, and compares its version with
    // the compiled-in TTC_VERSION. Emits exactly one of the signals below.
    void checkForUpdates();

    // Semantic-version comparison: true when remote is strictly newer than
    // local. Both are dotted numeric strings ("0.2.0"); a leading 'v' and any
    // pre-release suffix after '-' are ignored, missing components count as 0.
    static bool isNewer(const QString &remote, const QString &local);

signals:
    void updateAvailable(const UpdateInfo &info);
    void noUpdate();
    void checkFailed(const QString &err);

private:
    void onManifestFinished(QNetworkReply *reply);

    QNetworkAccessManager *m_net;
};
