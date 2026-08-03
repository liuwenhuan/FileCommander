#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include "update/UpdateChecker.h"
#include "update/Updater.h"

#include "version.h"

// Cover for the online-update client: the manifest contract, the network paths
// around it, and the download/verify half of applying an update.
//
// The install step itself is deliberately NOT exercised through Updater here --
// on Windows it would replace the test runner's own directory. It is covered
// two other ways: test_WindowsUpdateInstaller.cpp runs the real PowerShell
// installer against a scratch tree, and tools/mock-update-server.py drives a
// real binary end to end.
namespace {

QString sha256Of(const QByteArray &data) {
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// An event-driven HTTP peer. Event-driven rather than blocking because
// QNetworkAccessManager lives on the test's own thread: a server that blocked
// waiting for a connection would deadlock against the client it is serving.
class MockUpdateServer : public QObject {
public:
    struct Route {
        int status = 200;
        QByteArray contentType = "application/json";
        QByteArray body;
        int delayMs = 0;      // answer this late (exercises the client timeout)
        bool silent = false;  // accept the request and never answer at all
        bool dropAfterHeaders = false; // send the headers, then hang up mid-body
    };

    MockUpdateServer() {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, &MockUpdateServer::onConnection);
    }

    quint16 port() const { return m_server.serverPort(); }
    QString url(const QString &path) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path);
    }
    void setRoute(const QString &path, const Route &route) { m_routes.insert(path, route); }
    int requestCount(const QString &path) const { return m_hits.value(path); }
    QByteArray lastRequestHead() const { return m_lastHead; }

private:
    void onConnection() {
        QTcpSocket *sock = m_server.nextPendingConnection();
        if (!sock)
            return;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            m_pending[sock] += sock->readAll();
            if (!m_pending.value(sock).contains("\r\n\r\n"))
                return;
            const QByteArray head = m_pending.take(sock);
            m_lastHead = head;
            const QByteArray requestLine = head.left(head.indexOf("\r\n"));
            const QList<QByteArray> parts = requestLine.split(' ');
            const QString path = parts.size() > 1 ? QString::fromUtf8(parts.at(1)) : QString();
            m_hits[path] += 1;
            respond(sock, path);
        });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }

    void respond(QTcpSocket *sock, const QString &path) {
        if (!m_routes.contains(path)) {
            sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            sock->disconnectFromHost();
            return;
        }
        const Route route = m_routes.value(path);
        if (route.silent)
            return; // hold the connection open, say nothing

        auto send = [sock, route] {
            if (!sock || sock->state() != QAbstractSocket::ConnectedState)
                return;
            QByteArray head = "HTTP/1.1 " + QByteArray::number(route.status) + " X\r\n";
            head += "Content-Type: " + route.contentType + "\r\n";
            head += "Content-Length: " + QByteArray::number(route.body.size()) + "\r\n";
            head += "Connection: close\r\n\r\n";
            sock->write(head);
            if (route.dropAfterHeaders) {
                // Promise a body, deliver nothing, hang up: the client must not
                // treat a truncated download as a complete one.
                sock->flush();
                sock->abort();
                return;
            }
            sock->write(route.body);
            sock->flush();
            sock->disconnectFromHost();
        };

        if (route.delayMs > 0)
            QTimer::singleShot(route.delayMs, sock, send);
        else
            send();
    }

    QTcpServer m_server;
    QHash<QString, Route> m_routes;
    QHash<QString, int> m_hits;
    QHash<QTcpSocket *, QByteArray> m_pending;
    QByteArray m_lastHead;
};

// Records what it was asked to install instead of installing it, so the
// download and verification stages can be driven to completion.
class RecordingUpdater : public Updater {
public:
    using Updater::Updater;

    QString installedFrom;
    UpdateInfo installedInfo;
    QByteArray installedBytes;

protected:
    void install(const QString &downloadedFile, const UpdateInfo &info) override {
        installedFrom = downloadedFile;
        installedInfo = info;
        QFile file(downloadedFile);
        if (file.open(QIODevice::ReadOnly))
            installedBytes = file.readAll();
        emit finished(true, QStringLiteral("installed"));
    }
};

// Waits for the first of the checker's three outcome signals.
struct CheckOutcome {
    bool available = false;
    bool upToDate = false;
    bool failed = false;
    UpdateInfo info;
    QString error;
};

CheckOutcome runCheck(UpdateChecker &checker, int timeoutMs = 10000) {
    CheckOutcome out;
    QEventLoop loop;
    QObject::connect(&checker, &UpdateChecker::updateAvailable, &loop,
                     [&](const UpdateInfo &i) { out.available = true; out.info = i; loop.quit(); });
    QObject::connect(&checker, &UpdateChecker::noUpdate, &loop,
                     [&] { out.upToDate = true; loop.quit(); });
    QObject::connect(&checker, &UpdateChecker::checkFailed, &loop,
                     [&](const QString &e) { out.failed = true; out.error = e; loop.quit(); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    // Started from inside the loop: some outcomes (an unconfigured URL) are
    // decided synchronously, and a quit() that lands before exec() is ignored --
    // the test would then sit out the whole timeout and still pass, hiding the
    // fact that the answer was instant.
    QTimer::singleShot(0, &loop, [&checker] { checker.checkForUpdates(); });
    loop.exec();
    return out;
}

QByteArray manifestJson(const QString &version, const QString &segment, const QString &url,
                        const QString &sha256) {
    return QStringLiteral("{\n"
                          "  \"version\": \"%1\",\n"
                          "  \"date\": \"2026-08-03\",\n"
                          "  \"notes\": \"line one\\nline two\",\n"
                          "  \"%2\": { \"url\": \"%3\", \"sha256\": \"%4\" }\n"
                          "}")
        .arg(version, segment, url, sha256)
        .toUtf8();
}

const QString kAnyUrl = QStringLiteral("https://example.com/pkg.zip");
const QString kAnyHash = QString(64, QLatin1Char('a'));

} // namespace

// --- version comparison ----------------------------------------------------

TEST(UpdateVersionTest, ComparesComponentwiseAndIgnoresDecorations) {
    EXPECT_TRUE(UpdateChecker::isNewer("0.2.1", "0.2.0"));
    EXPECT_TRUE(UpdateChecker::isNewer("0.10.0", "0.9.9")); // not string order
    EXPECT_TRUE(UpdateChecker::isNewer("1.0", "0.99.99"));
    EXPECT_TRUE(UpdateChecker::isNewer("v0.3.0", "0.2.0")); // leading v
    EXPECT_TRUE(UpdateChecker::isNewer("0.2.1-beta", "0.2.0")); // pre-release suffix

    EXPECT_FALSE(UpdateChecker::isNewer("0.2.0", "0.2.0"));
    EXPECT_FALSE(UpdateChecker::isNewer("0.2.0", "0.2.0.0")); // missing parts are zero
    EXPECT_FALSE(UpdateChecker::isNewer("0.1.9", "0.2.0"));
    // A downgrade is not an update, however it is dressed up.
    EXPECT_FALSE(UpdateChecker::isNewer("0.1.0", "9.9.9"));
}

// --- the manifest contract -------------------------------------------------

TEST(UpdateManifestTest, AcceptsAWellFormedNewerRelease) {
    UpdateInfo info;
    QString error;
    const auto result = UpdateChecker::parseManifest(
        manifestJson("0.3.0", "windows", kAnyUrl, kAnyHash), "0.2.0", "windows", &info, &error);

    ASSERT_EQ(result, UpdateChecker::ParseResult::UpdateAvailable) << error.toStdString();
    EXPECT_EQ(info.version, QStringLiteral("0.3.0"));
    EXPECT_EQ(info.date, QStringLiteral("2026-08-03"));
    EXPECT_EQ(info.notes, QStringLiteral("line one\nline two")); // \n survives JSON escaping
    EXPECT_EQ(info.url, kAnyUrl);
    EXPECT_EQ(info.sha256, kAnyHash);
}

TEST(UpdateManifestTest, SameOrOlderVersionIsUpToDateNotAnError) {
    QString error;
    EXPECT_EQ(UpdateChecker::parseManifest(manifestJson("0.2.0", "windows", kAnyUrl, kAnyHash),
                                           "0.2.0", "windows", nullptr, &error),
              UpdateChecker::ParseResult::UpToDate);
    EXPECT_EQ(UpdateChecker::parseManifest(manifestJson("0.1.0", "windows", kAnyUrl, kAnyHash),
                                           "0.2.0", "windows", nullptr, &error),
              UpdateChecker::ParseResult::UpToDate);
}

// An up-to-date answer must not depend on the package segments being present or
// even valid: a manifest that has dropped the segment for this platform still
// means "nothing newer" when its version is not newer.
TEST(UpdateManifestTest, UpToDateIsDecidedBeforeTheSegmentIsLookedAt) {
    QString error;
    EXPECT_EQ(UpdateChecker::parseManifest("{\"version\": \"0.2.0\"}", "0.2.0", "windows",
                                           nullptr, &error),
              UpdateChecker::ParseResult::UpToDate);
}

TEST(UpdateManifestTest, RejectsAManifestWithNoPackageForThisForm) {
    UpdateInfo info;
    QString error;
    // A newer release that only ships a deb, seen by a Windows build.
    EXPECT_EQ(UpdateChecker::parseManifest(manifestJson("0.3.0", "deb", kAnyUrl, kAnyHash),
                                           "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::Invalid);
    EXPECT_TRUE(error.contains(QStringLiteral("windows"))) << error.toStdString();
}

TEST(UpdateManifestTest, RejectsMalformedJsonAndMissingVersion) {
    UpdateInfo info;
    QString error;
    EXPECT_EQ(UpdateChecker::parseManifest("{not json", "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::Invalid);
    EXPECT_FALSE(error.isEmpty());

    EXPECT_EQ(UpdateChecker::parseManifest("{\"date\": \"2026-08-03\"}", "0.2.0", "windows",
                                           &info, &error),
              UpdateChecker::ParseResult::Invalid);
    EXPECT_EQ(UpdateChecker::parseManifest("[]", "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::Invalid);
    // A version field that carries no numbers at all would compare as 0.0.0 and
    // silently read as "up to date", hiding a broken manifest.
    EXPECT_EQ(UpdateChecker::parseManifest("{\"version\": \"latest\"}", "0.2.0", "windows",
                                           &info, &error),
              UpdateChecker::ParseResult::Invalid);
}

// The manifest is remote input, and what it names gets downloaded and then
// executed. A scheme we did not intend is refused outright rather than tried.
TEST(UpdateManifestTest, RejectsDownloadUrlsThatAreNotHttp) {
    UpdateInfo info;
    QString error;
    for (const QString &url : {QStringLiteral("file:///etc/passwd"),
                               QStringLiteral("ftp://example.com/pkg.zip"),
                               QStringLiteral("\\\\evil-host\\share\\pkg.zip"),
                               QStringLiteral("/tmp/pkg.zip"),
                               QStringLiteral("https:///pkg.zip")}) {
        SCOPED_TRACE(url.toStdString());
        EXPECT_EQ(UpdateChecker::parseManifest(manifestJson("0.3.0", "windows", url, kAnyHash),
                                               "0.2.0", "windows", &info, &error),
                  UpdateChecker::ParseResult::Invalid);
    }
}

// A hash that cannot possibly match is a manifest bug, and saying so now beats
// discovering it after a several-hundred-megabyte download.
TEST(UpdateManifestTest, RejectsAMalformedSha256) {
    UpdateInfo info;
    QString error;
    for (const QString &hash : {QStringLiteral("deadbeef"),          // too short
                                QString(65, QLatin1Char('a')),       // too long
                                QString(64, QLatin1Char('z')),       // not hex
                                QStringLiteral("sha256:") + kAnyHash}) {
        SCOPED_TRACE(hash.toStdString());
        EXPECT_EQ(UpdateChecker::parseManifest(manifestJson("0.3.0", "windows", kAnyUrl, hash),
                                               "0.2.0", "windows", &info, &error),
                  UpdateChecker::ParseResult::Invalid);
    }
    // Upper case hex is still hex.
    EXPECT_EQ(UpdateChecker::parseManifest(
                  manifestJson("0.3.0", "windows", kAnyUrl, QString(64, QLatin1Char('A'))),
                  "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::UpdateAvailable);
}

TEST(UpdateManifestTest, SegmentKeyMatchesHowThisBuildInstalls) {
#ifdef Q_OS_WIN
    EXPECT_EQ(UpdateChecker::packageSegmentKey(), QStringLiteral("windows"));
#else
    EXPECT_EQ(UpdateChecker::packageSegmentKey(),
              Updater::runningAsAppImage() ? QStringLiteral("appimage") : QStringLiteral("deb"));
#endif
}

// --- the checker over a real socket ----------------------------------------

TEST(UpdateCheckerNetworkTest, ReportsAnAvailableReleaseFromTheServer) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.body = manifestJson("99.0.0", UpdateChecker::packageSegmentKey(), kAnyUrl, kAnyHash);
    server.setRoute(QStringLiteral("/version.json"), route);

    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    const CheckOutcome out = runCheck(checker);

    ASSERT_TRUE(out.available) << out.error.toStdString();
    EXPECT_EQ(out.info.version, QStringLiteral("99.0.0"));
    // The manifest must never be served from a cache: a stale copy hides a
    // release for as long as the proxy keeps it.
    EXPECT_TRUE(server.lastRequestHead().contains("Cache-Control: no-cache"));
    EXPECT_TRUE(server.lastRequestHead().contains("FileCommander/" TTC_VERSION));
}

TEST(UpdateCheckerNetworkTest, ReportsUpToDateAgainstTheRunningVersion) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.body = manifestJson(QStringLiteral(TTC_VERSION), UpdateChecker::packageSegmentKey(),
                              kAnyUrl, kAnyHash);
    server.setRoute(QStringLiteral("/version.json"), route);

    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    const CheckOutcome out = runCheck(checker);

    EXPECT_TRUE(out.upToDate);
    EXPECT_FALSE(out.available);
}

TEST(UpdateCheckerNetworkTest, AnHttpErrorIsReportedNotSwallowed) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.status = 500;
    route.body = "boom";
    server.setRoute(QStringLiteral("/version.json"), route);

    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    const CheckOutcome out = runCheck(checker);

    EXPECT_TRUE(out.failed);
    EXPECT_FALSE(out.error.isEmpty());
}

TEST(UpdateCheckerNetworkTest, AMissingManifestIsReported) {
    MockUpdateServer server; // no routes at all -> 404
    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    const CheckOutcome out = runCheck(checker);

    EXPECT_TRUE(out.failed);
}

// A server that accepts the connection and then says nothing must not leave the
// check pending forever: neither signal would ever fire, and the daily check
// would leak one checker per launch.
TEST(UpdateCheckerNetworkTest, ASilentServerTimesOutInsteadOfHangingForever) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.silent = true;
    server.setRoute(QStringLiteral("/version.json"), route);

    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    checker.setTimeoutMs(600);

    QElapsedTimer clock;
    clock.start();
    const CheckOutcome out = runCheck(checker, 8000);

    EXPECT_TRUE(out.failed) << "the check never finished";
    EXPECT_LT(clock.elapsed(), 6000) << "took " << clock.elapsed() << "ms";
    EXPECT_TRUE(out.error.contains(QStringLiteral("did not respond")))
        << out.error.toStdString();
}

// The placeholder host is reserved and never resolves. Saying "not configured"
// beats a DNS error the user cannot act on.
TEST(UpdateCheckerNetworkTest, AnUnconfiguredBuildSaysSoWithoutAskingTheNetwork) {
    if (UpdateChecker::manifestUrlIsConfigured())
        GTEST_SKIP() << "this build has a real update server configured";

    UpdateChecker checker;
    const CheckOutcome out = runCheck(checker);
    EXPECT_TRUE(out.failed);
    EXPECT_TRUE(out.error.contains(QStringLiteral("No update server")))
        << out.error.toStdString();
}

TEST(UpdateCheckerNetworkTest, TheEnvironmentVariableOverridesTheCompiledInUrl) {
    const QByteArray previous = qgetenv("FILECOMMANDER_UPDATE_MANIFEST_URL");
    qputenv("FILECOMMANDER_UPDATE_MANIFEST_URL", "http://127.0.0.1:9/version.json");
    EXPECT_EQ(UpdateChecker::manifestUrl(),
              QStringLiteral("http://127.0.0.1:9/version.json"));
    EXPECT_TRUE(UpdateChecker::manifestUrlIsConfigured());

    if (previous.isEmpty())
        qunsetenv("FILECOMMANDER_UPDATE_MANIFEST_URL");
    else
        qputenv("FILECOMMANDER_UPDATE_MANIFEST_URL", previous);
}

// --- downloading and verifying ---------------------------------------------

namespace {

struct ApplyOutcome {
    bool sawFinished = false;
    bool ok = false;
    QString message;
    QVector<int> progress;
};

ApplyOutcome runApply(Updater &updater, const UpdateInfo &info, int timeoutMs = 10000,
                      const std::function<void()> &afterStart = {}) {
    ApplyOutcome out;
    QEventLoop loop;
    QObject::connect(&updater, &Updater::finished, &loop,
                     [&](bool ok, const QString &message) {
                         out.sawFinished = true;
                         out.ok = ok;
                         out.message = message;
                         loop.quit();
                     });
    QObject::connect(&updater, &Updater::progress, &loop,
                     [&](int percent) { out.progress.append(percent); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    QTimer::singleShot(0, &loop, [&updater, info, afterStart] {
        updater.apply(info); // see runCheck: synchronous failures need the loop running
        if (afterStart)
            afterStart();
    });
    loop.exec();
    return out;
}

UpdateInfo packageInfo(const MockUpdateServer &server, const QByteArray &payload,
                       const QString &hash) {
    UpdateInfo info;
    info.version = QStringLiteral("99.0.0");
    info.url = server.url(QStringLiteral("/pkg.zip"));
    info.sha256 = hash;
    return info;
}

} // namespace

TEST(UpdaterDownloadTest, DownloadsVerifiesAndHandsTheExactBytesToTheInstaller) {
    const QByteArray payload = QByteArray("PK\x03\x04").append(4096, 'x');
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.contentType = "application/zip";
    route.body = payload;
    server.setRoute(QStringLiteral("/pkg.zip"), route);

    RecordingUpdater updater;
    const ApplyOutcome out = runApply(updater, packageInfo(server, payload, sha256Of(payload)));

    ASSERT_TRUE(out.sawFinished);
    EXPECT_TRUE(out.ok) << out.message.toStdString();
    EXPECT_EQ(updater.installedBytes, payload) << "the installer was handed different bytes";
    EXPECT_EQ(updater.installedInfo.version, QStringLiteral("99.0.0"));
    // The suffix has to survive: the Windows installer refuses anything but a
    // .zip, and dpkg refuses anything but a .deb.
    EXPECT_TRUE(updater.installedFrom.endsWith(QStringLiteral(".zip")))
        << updater.installedFrom.toStdString();
    EXPECT_FALSE(out.progress.isEmpty());
}

// The security boundary of the whole feature: nothing is installed unless the
// bytes hash to what the manifest said.
TEST(UpdaterDownloadTest, AChecksumMismatchAbortsBeforeInstalling) {
    const QByteArray payload = "the wrong bytes entirely";
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.body = payload;
    server.setRoute(QStringLiteral("/pkg.zip"), route);

    RecordingUpdater updater;
    const ApplyOutcome out = runApply(updater, packageInfo(server, payload, kAnyHash));

    ASSERT_TRUE(out.sawFinished);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(out.message.contains(QStringLiteral("Checksum"))) << out.message.toStdString();
    EXPECT_TRUE(updater.installedFrom.isEmpty()) << "install ran on an unverified package";
}

// A truncated body hashes differently, so the same guard has to catch it -- the
// point being that "the connection ended" is never read as "the file is whole".
TEST(UpdaterDownloadTest, ATruncatedDownloadNeverReachesTheInstaller) {
    const QByteArray payload = QByteArray(65536, 'z');
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.body = payload;
    route.dropAfterHeaders = true;
    server.setRoute(QStringLiteral("/pkg.zip"), route);

    RecordingUpdater updater;
    const ApplyOutcome out = runApply(updater, packageInfo(server, payload, sha256Of(payload)));

    ASSERT_TRUE(out.sawFinished);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(updater.installedFrom.isEmpty());
}

TEST(UpdaterDownloadTest, AServerErrorIsReportedAndInstallsNothing) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.status = 500;
    route.body = "no";
    server.setRoute(QStringLiteral("/pkg.zip"), route);

    RecordingUpdater updater;
    const ApplyOutcome out = runApply(updater, packageInfo(server, QByteArray(), kAnyHash));

    ASSERT_TRUE(out.sawFinished);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(updater.installedFrom.isEmpty());
}

TEST(UpdaterDownloadTest, IncompleteUpdateInfoFailsImmediately) {
    RecordingUpdater updater;
    UpdateInfo info;
    info.version = QStringLiteral("99.0.0"); // no url, no hash
    const ApplyOutcome out = runApply(updater, info, 2000);

    ASSERT_TRUE(out.sawFinished);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(updater.installedFrom.isEmpty());
}

// A download that goes quiet has to end by itself, or the dialog sits on
// "Downloading…" until the user kills the app.
TEST(UpdaterDownloadTest, AStalledDownloadGivesUpOnItsOwn) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.silent = true;
    server.setRoute(QStringLiteral("/pkg.zip"), route);

    RecordingUpdater updater;
    updater.setStallTimeoutMs(600);

    QElapsedTimer clock;
    clock.start();
    const ApplyOutcome out = runApply(updater, packageInfo(server, QByteArray(), kAnyHash), 8000);

    ASSERT_TRUE(out.sawFinished) << "the download never gave up";
    EXPECT_FALSE(out.ok);
    EXPECT_LT(clock.elapsed(), 6000) << "took " << clock.elapsed() << "ms";
    EXPECT_TRUE(updater.installedFrom.isEmpty());
}

TEST(UpdaterDownloadTest, CancellingStopsTheDownloadAndInstallsNothing) {
    MockUpdateServer server;
    MockUpdateServer::Route route;
    route.body = QByteArray(1 << 20, 'q');
    route.delayMs = 4000; // still in flight when we cancel
    server.setRoute(QStringLiteral("/pkg.zip"), route);

    RecordingUpdater updater;
    const ApplyOutcome out = runApply(updater, packageInfo(server, QByteArray(), kAnyHash), 9000,
                                      [&updater] { updater.cancel(); });

    ASSERT_TRUE(out.sawFinished);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(out.message.contains(QStringLiteral("cancel"), Qt::CaseInsensitive))
        << out.message.toStdString();
    EXPECT_TRUE(updater.installedFrom.isEmpty());
}
