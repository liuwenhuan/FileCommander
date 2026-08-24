#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

// Announcement metadata from the trusted release manifest. FileCommander never
// downloads or installs a package itself; the fixed update page lists packages.
struct UpdateInfo {
    QString version;
    QString notes;
    QString date;
};

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    void checkForUpdates();

    // The official production endpoint. Tests can use setManifestUrl() to
    // inject a local server without weakening production URL validation.
    static QString manifestUrl();
    static bool manifestUrlIsConfigured();
    static QString updatePageUrl();

    void setManifestUrl(const QString &url);
    void setTimeoutMs(int ms);

    static bool isNewer(const QString &remote, const QString &local);

    enum class ParseResult { UpdateAvailable, UpToDate, Invalid };

    // segmentKey remains a harmless compatibility parameter for existing test
    // seams; schema 2 desktop manifests do not resolve package URLs in-app.
    static ParseResult parseManifest(const QByteArray &body, const QString &localVersion,
                                     const QString &segmentKey, UpdateInfo *info,
                                     QString *error);

signals:
    void updateAvailable(const UpdateInfo &info);
    void noUpdate();
    void checkFailed(const QString &err);

private:
    void onManifestFinished(QNetworkReply *reply);
    void finishFailed(const QString &error);

    QNetworkAccessManager *m_net;
    QString m_manifestUrl;
    int m_timeoutMs = 15000;
    QTimer *m_timeout = nullptr;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_manifestBody;
    qint64 m_manifestBytes = 0;
    bool m_tooLarge = false;
};
