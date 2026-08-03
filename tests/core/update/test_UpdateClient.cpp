#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "update/UpdateChecker.h"

#include "version.h"

// Cover for the update check: the manifest contract and the network paths
// around it. That is the whole client -- the application announces a release
// and points at where to get it; fetching and installing the package is the
// Microsoft Store's job, or the user's (see tests/ui/test_UpdateUi.cpp for
// what it puts in front of them).
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
            respond(sock, path, head);
        });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }

    void respond(QTcpSocket *sock, const QString &path, const QByteArray &head) {
        Q_UNUSED(head);
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

TEST(UpdateManifestTest, SegmentKeyMatchesHowThisBuildWasInstalled) {
#ifdef Q_OS_WIN
    EXPECT_EQ(UpdateChecker::packageSegmentKey(), QStringLiteral("windows"));
#else
    EXPECT_EQ(UpdateChecker::packageSegmentKey(),
              UpdateChecker::runningAsAppImage() ? QStringLiteral("appimage")
                                                 : QStringLiteral("deb"));
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

// --- the optional store link -----------------------------------------------
//
// The client announces a release and hands the user the address; if the
// manifest names a store page, it offers that too. Neither is fetched here --
// they end up in QDesktopServices::openUrl, which is why the same http(s)-only
// rule that guards the package URL guards this one.

TEST(UpdateManifestTest, PicksUpAStoreLinkFromTheSegmentOrTheRoot) {
    UpdateInfo info;
    QString error;

    const QByteArray perSegment =
        "{\"version\": \"0.3.0\","
        " \"storeUrl\": \"https://shared.example.com/app\","
        " \"windows\": {\"url\": \"https://example.com/pkg.zip\", \"sha256\": \""
        + kAnyHash.toUtf8() + "\", \"storeUrl\": \"https://apps.microsoft.com/detail/x\"}}";
    ASSERT_EQ(UpdateChecker::parseManifest(perSegment, "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::UpdateAvailable)
        << error.toStdString();
    EXPECT_EQ(info.storeUrl, QStringLiteral("https://apps.microsoft.com/detail/x"))
        << "the platform's own store link must win over the shared one";

    const QByteArray rootOnly = "{\"version\": \"0.3.0\","
                                " \"storeUrl\": \"https://shared.example.com/app\","
                                " \"windows\": {\"url\": \"https://example.com/pkg.zip\","
                                " \"sha256\": \"" + kAnyHash.toUtf8() + "\"}}";
    ASSERT_EQ(UpdateChecker::parseManifest(rootOnly, "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::UpdateAvailable);
    EXPECT_EQ(info.storeUrl, QStringLiteral("https://shared.example.com/app"));
}

TEST(UpdateManifestTest, AStoreLinkIsOptionalAndAnUnusableOneIsDropped) {
    UpdateInfo info;
    QString error;

    // Absent: the release is still perfectly valid, there is just no button.
    ASSERT_EQ(UpdateChecker::parseManifest(manifestJson("0.3.0", "windows", kAnyUrl, kAnyHash),
                                           "0.2.0", "windows", &info, &error),
              UpdateChecker::ParseResult::UpdateAvailable);
    EXPECT_TRUE(info.storeUrl.isEmpty());

    // Present but not something we are willing to open. Dropping it beats
    // rejecting the whole manifest: the release itself is still fine.
    for (const QString &bad : {QStringLiteral("file:///etc/passwd"),
                               QStringLiteral("javascript:alert(1)"),
                               QStringLiteral("ms-windows-store://pdp/?productid=x")}) {
        SCOPED_TRACE(bad.toStdString());
        const QByteArray body = "{\"version\": \"0.3.0\", \"storeUrl\": \"" + bad.toUtf8()
                                + "\", \"windows\": {\"url\": \"https://example.com/pkg.zip\","
                                  " \"sha256\": \"" + kAnyHash.toUtf8() + "\"}}";
        ASSERT_EQ(UpdateChecker::parseManifest(body, "0.2.0", "windows", &info, &error),
                  UpdateChecker::ParseResult::UpdateAvailable)
            << error.toStdString();
        EXPECT_TRUE(info.storeUrl.isEmpty());
    }
}
