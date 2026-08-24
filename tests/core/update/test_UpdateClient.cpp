#include <gtest/gtest.h>

#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include "MockHttpServer.h"
#include "update/UpdateChecker.h"
#include "version.h"

namespace {

QByteArray manifest(const QString &version, const QString &date = QStringLiteral("2026-08-24"),
                    const QString &notes = QStringLiteral("release notes")) {
    return QStringLiteral("{\"schema\":2,\"version\":\"%1\",\"date\":\"%2\",\"notes\":\"%3\"}")
        .arg(version, date, notes).toUtf8();
}

struct Outcome {
    bool available = false;
    bool current = false;
    QString error;
    UpdateInfo info;
};

Outcome check(UpdateChecker &checker) {
    Outcome result;
    QEventLoop loop;
    QObject::connect(&checker, &UpdateChecker::updateAvailable, &loop, [&](const UpdateInfo &info) {
        result.available = true;
        result.info = info;
        loop.quit();
    });
    QObject::connect(&checker, &UpdateChecker::noUpdate, &loop, [&] {
        result.current = true;
        loop.quit();
    });
    QObject::connect(&checker, &UpdateChecker::checkFailed, &loop, [&](const QString &error) {
        result.error = error;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    QTimer::singleShot(0, &loop, [&] { checker.checkForUpdates(); });
    loop.exec();
    return result;
}

} // namespace

TEST(UpdateVersionTest, ComparesStrictDottedNumericVersions) {
    EXPECT_TRUE(UpdateChecker::isNewer("0.2.1", "0.2.0"));
    EXPECT_TRUE(UpdateChecker::isNewer("0.10.0", "0.9.9"));
    EXPECT_FALSE(UpdateChecker::isNewer("1.0", "0.9.9"));
    EXPECT_FALSE(UpdateChecker::isNewer("1.0.0.0", "0.9.9"));
    EXPECT_FALSE(UpdateChecker::isNewer("1..2", "1.0.0"));
    EXPECT_FALSE(UpdateChecker::isNewer("1.0.0-beta", "1.0.0"));
    EXPECT_FALSE(UpdateChecker::isNewer("0.1.9", "0.2.0"));
}

TEST(UpdateManifestTest, RequiresSchemaVersionDateAndNotes) {
    UpdateInfo info;
    QString error;
    ASSERT_EQ(UpdateChecker::parseManifest(manifest("9.9.9"), QStringLiteral(TTC_VERSION), {}, &info, &error),
              UpdateChecker::ParseResult::UpdateAvailable) << error.toStdString();
    EXPECT_EQ(info.version, QStringLiteral("9.9.9"));
    EXPECT_EQ(info.date, QStringLiteral("2026-08-24"));
    EXPECT_EQ(info.notes, QStringLiteral("release notes"));
    for (const QByteArray &bad : {QByteArray("{}"), QByteArray("{\"schema\":1,\"version\":\"9.9.9\",\"date\":\"2026-08-24\",\"notes\":\"x\"}"),
                                  QByteArray("{\"schema\":2.5,\"version\":\"9.9.9\",\"date\":\"2026-08-24\",\"notes\":\"x\"}"),
                                  QByteArray("{\"schema\":\"2\",\"version\":\"9.9.9\",\"date\":\"2026-08-24\",\"notes\":\"x\"}"),
                                  QByteArray("{\"schema\":2,\"version\":\"9.9\",\"date\":\"2026-08-24\",\"notes\":\"x\"}"),
                                  QByteArray("{\"schema\":2,\"version\":\"999999999999.0\",\"date\":\"2026-08-24\",\"notes\":\"x\"}"),
                                  QByteArray("{\"schema\":2,\"version\":\"9.9.9\",\"date\":\"bad\",\"notes\":\"x\"}")}) {
        EXPECT_EQ(UpdateChecker::parseManifest(bad, QStringLiteral(TTC_VERSION), {}, &info, &error),
                  UpdateChecker::ParseResult::Invalid);
    }
}

TEST(UpdateCheckerNetworkTest, InjectedLocalServerReportsAvailability) {
    MockHttpServer server;
    MockHttpServer::Route route;
    route.body = manifest("99.0.0");
    server.setRoute(QStringLiteral("/version.json"), route);
    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    const Outcome result = check(checker);
    ASSERT_TRUE(result.available) << result.error.toStdString();
    EXPECT_EQ(result.info.version, QStringLiteral("99.0.0"));
    EXPECT_TRUE(server.lastRequestHead().contains("Cache-Control: no-cache"));
}

TEST(UpdateCheckerNetworkTest, RejectsOversizedManifest) {
    MockHttpServer server;
    MockHttpServer::Route route;
    route.body = QByteArray(65 * 1024, 'x');
    server.setRoute(QStringLiteral("/version.json"), route);
    UpdateChecker checker;
    checker.setManifestUrl(server.url(QStringLiteral("/version.json")));
    const Outcome result = check(checker);
    EXPECT_FALSE(result.error.isEmpty());
}

TEST(UpdateCheckerTest, UsesFixedOfficialEndpoints) {
    EXPECT_EQ(UpdateChecker::manifestUrl(), QStringLiteral("https://fc.aigutta.com/version.json"));
    EXPECT_EQ(UpdateChecker::updatePageUrl(), QStringLiteral("https://fc.aigutta.com/update.html"));
}
