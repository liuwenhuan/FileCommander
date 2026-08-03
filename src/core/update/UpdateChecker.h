#pragma once

#include <QObject>
#include <QString>

// One release as advertised by the server manifest (version.json), already
// resolved to the package matching this install's form (AppImage, deb or the
// Windows portable ZIP). See docs/UPDATE_SERVER.md for the manifest schema.
struct UpdateInfo {
    QString version; // e.g. "0.2.0"
    QString notes;   // human-readable release notes / changelog
    QString date;    // release date, e.g. "2026-07-20"
    QString url;     // download URL for the matching package
    QString sha256;  // SHA-256 of the package, for the user to check their own
                     // download against (hex, case-insensitive)
    QString storeUrl; // optional: where this platform's store lists the app
};

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Fetches the server manifest and reports whether a newer release exists.
// That is the whole of it: the application announces a release and points at
// where to get it (see UpdateDialog); fetching and installing the package is
// the Microsoft Store's job, or the user's.
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // GETs the manifest, parses it, and compares its version with the
    // compiled-in TTC_VERSION. Emits exactly one of the signals below -- always
    // exactly one, including on timeout.
    void checkForUpdates();

    // The manifest URL this run will use: the compiled-in default
    // (-DFILECOMMANDER_UPDATE_MANIFEST_URL at configure time) unless the
    // environment variable FILECOMMANDER_UPDATE_MANIFEST_URL overrides it. The
    // override exists so a build can be pointed at a local test server without
    // recompiling; it is read once per call, never cached.
    static QString manifestUrl();

    // True when the effective URL is still the placeholder, i.e. nobody has
    // configured a real update server for this build. Checked before any
    // request so the user gets "not configured" instead of a DNS error.
    static bool manifestUrlIsConfigured();

    // Overrides the URL for this instance only (tests, diagnostics).
    void setManifestUrl(const QString &url);

    // How long to wait for the manifest before giving up. Default 15s. A server
    // that accepts the connection and then says nothing must not leave the
    // check pending forever -- neither signal would ever fire, and the daily
    // check would leak a checker per launch.
    void setTimeoutMs(int ms);

    // Semantic-version comparison: true when remote is strictly newer than
    // local. Both are dotted numeric strings ("0.2.0"); a leading 'v' and any
    // pre-release suffix after '-' are ignored, missing components count as 0.
    static bool isNewer(const QString &remote, const QString &local);

    // Which manifest segment describes this install: "windows", "appimage" or
    // "deb". Decided at runtime because one Linux binary can be either form.
    static QString packageSegmentKey();

    // True when launched from an AppImage bundle (its runtime exports $APPIMAGE
    // pointing at the .AppImage file). Only packageSegmentKey() needs this, but
    // it is the one question the segment choice cannot answer at compile time.
    static bool runningAsAppImage();

    enum class ParseResult {
        UpdateAvailable, // info filled in
        UpToDate,        // manifest is valid, but not newer than local
        Invalid,         // error filled in
    };

    // Parses a manifest body against `localVersion`, resolving `segmentKey`.
    // Pure: no network, no globals. The whole manifest contract lives here so
    // it can be tested exhaustively without a server.
    static ParseResult parseManifest(const QByteArray &body, const QString &localVersion,
                                     const QString &segmentKey, UpdateInfo *info,
                                     QString *error);

signals:
    void updateAvailable(const UpdateInfo &info);
    void noUpdate();
    void checkFailed(const QString &err);

private:
    void onManifestFinished(QNetworkReply *reply);

    QNetworkAccessManager *m_net;
    QString m_manifestUrl;
    int m_timeoutMs = 15000;
    QTimer *m_timeout = nullptr;
};
