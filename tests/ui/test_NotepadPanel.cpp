#include <gtest/gtest.h>

#include <QApplication>
#include <QAbstractItemView>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QUrl>

#include <utility>

#include "MockHttpServer.h"
#include "TryUntil.h"
#include "CloudClipboardController.h"
#include "CloudClipboardRowDelegate.h"
#include "NotepadPanel.h"
#include "account/AccountClient.h"
#include "account/ClipboardHistoryStore.h"
#include "account/DeviceAgent.h"
#include "config/Settings.h"

namespace {

MockHttpServer::Route json(const QByteArray &body, int status = 200) {
    MockHttpServer::Route route;
    route.status = status;
    route.body = body;
    return route;
}

void signIn(AccountClient &client, MockHttpServer &server) {
    server.setRoute(QStringLiteral("/v1/auth/login"),
                    json(R"({"access_token":"access-1","refresh_token":"refresh-1","device_id":"device-1"})"));
    QSignalSpy loggedIn(&client, &AccountClient::loggedIn);
    client.login(QStringLiteral("clipboard@example.com"), QStringLiteral("password"),
                 QStringLiteral("test device"), QString());
    FC_TRY_COMPARE_WITH_TIMEOUT(loggedIn.count(), 1, 5000);
}

QByteArray pngBytes(QImage *image = nullptr) {
    QImage generated(7, 5, QImage::Format_ARGB32);
    generated.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    EXPECT_TRUE(buffer.open(QIODevice::WriteOnly));
    EXPECT_TRUE(generated.save(&buffer, "PNG"));
    if (image)
        *image = generated;
    return bytes;
}

QByteArray deliveryListJson(const QString &id, const QString &kind, const QByteArray &content,
                            const QImage &image = QImage()) {
    const QByteArray hash = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    QByteArray json = "{\"deliveries\":[{\"id\":\"" + id.toUtf8() + "\",\"payload_id\":\"payload-" +
                      id.toUtf8() + "\",\"kind\":\"" + kind.toUtf8() + "\",\"mime\":\"" +
                      (kind == QLatin1String("image") ? QByteArray("image/png") : QByteArray("text/plain")) +
                      "\",\"size\":" + QByteArray::number(content.size()) +
                      ",\"sha256\":\"" + hash +
                      "\",\"source_device_id\":\"device-2\",\"source_device_name\":\"other device\",\"created\":\"2026-08-23T12:00:00Z\"";
    if (!image.isNull())
        json += ",\"width\":" + QByteArray::number(image.width()) + ",\"height\":" +
                QByteArray::number(image.height());
    return json + "}]}";
}

void serveDeliveryContent(MockHttpServer &server, const QString &id, const QByteArray &content,
                          const QByteArray &hash = QByteArray()) {
    MockHttpServer::Route route;
    route.contentType = "image/png";
    route.body = content;
    route.headers.insert("X-Content-Sha256", hash.isEmpty()
                                                 ? QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()
                                                 : hash);
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content"), route);
}

} // namespace

TEST(CloudClipboardControllerTest, RejectsFileUrlsAndOversizedText) {
    QMimeData urls;
    urls.setUrls({QUrl::fromLocalFile(QStringLiteral("/private/file"))});
    EXPECT_FALSE(CloudClipboardController::acceptsText(&urls));

    QMimeData large;
    large.setText(QString(64 * 1024 + 1, QLatin1Char('x')));
    EXPECT_FALSE(CloudClipboardController::acceptsText(&large));

    QMimeData text;
    text.setText(QStringLiteral("safe text"));
    QString accepted;
    EXPECT_TRUE(CloudClipboardController::acceptsText(&text, &accepted));
    EXPECT_EQ(accepted, QStringLiteral("safe text"));
}

TEST(CloudClipboardControllerTest, AcceptsBrowserImageWithTextFallback) {
    QImage source(32, 24, QImage::Format_ARGB32);
    source.fill(Qt::red);
    QByteArray png;
    QBuffer buffer(&png);
    ASSERT_TRUE(buffer.open(QIODevice::WriteOnly));
    ASSERT_TRUE(source.save(&buffer, "PNG"));

    QMimeData mime;
    mime.setData(QStringLiteral("image/png"), png);
    mime.setText(QStringLiteral("https://example.com/image.png"));
    mime.setHtml(QStringLiteral("<img src=\"https://example.com/image.png\">"));
    QImage accepted;
    EXPECT_TRUE(CloudClipboardController::acceptsImage(&mime, &accepted));
    EXPECT_EQ(accepted.size(), source.size());
}

TEST(CloudClipboardControllerTest, AutomaticallyCapturesLocalClipboardWithoutPublishing) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    CloudClipboardController controller(store, &client);
    QSignalSpy changed(&controller, &CloudClipboardController::changed);
    const QString text = QStringLiteral("local capture %1").arg(QUuid::createUuid().toString());

    QApplication::clipboard()->setText(text);
    FC_TRY_COMPARE_WITH_TIMEOUT(controller.items().size(), 1, 1000);

    const ClipboardHistoryRecord &record = controller.items().first();
    EXPECT_EQ(record.origin, ClipboardRecordOrigin::Local);
    EXPECT_EQ(record.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(record.text, text);
    EXPECT_TRUE(changed.count() > 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard")), 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/session")), 0);
}

TEST(CloudClipboardControllerTest, CapturesCopiedLocalImageFileAsImageHistory) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("clipboard-file-image.png"));
    QImage source(36, 28, QImage::Format_ARGB32);
    source.fill(Qt::cyan);
    ASSERT_TRUE(source.save(imagePath, "PNG"));

    ClipboardHistoryStore store(directory.path());
    CloudClipboardController controller(store, nullptr);
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(imagePath)});
    QApplication::clipboard()->setMimeData(mime);

    FC_TRY_COMPARE_WITH_TIMEOUT(controller.items().size(), 1, 2000);
    const ClipboardHistoryRecord &record = controller.items().first();
    EXPECT_EQ(record.kind, ClipboardRecordKind::Image);
    EXPECT_EQ(record.width, source.width());
    EXPECT_EQ(record.height, source.height());
    EXPECT_TRUE(QFile::exists(record.imagePath));
}

TEST(CloudClipboardControllerTest, AutoSendDefaultsOffAndOnlyFutureLocalCaptureUsesTarget) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    ASSERT_FALSE(store.addLocalText(QStringLiteral("existing history")).id.isEmpty());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    server.setRoute(QStringLiteral("/v1/clipboard/send-targeted?target=device-2"),
                    json(R"({"payload_id":"targeted","recipient_count":1})"));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("offline target"), {}, false, false}});
    EXPECT_FALSE(controller.autoSendEnabled());
    controller.setSelectedTargetDeviceId(QStringLiteral("device-2"));
    controller.setAutoSendEnabled(true);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send-targeted?target=device-2")), 0);

    QApplication::clipboard()->setText(QStringLiteral("future targeted auto send"));
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/send-targeted?target=device-2")), 1, 5000);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 0);
    EXPECT_EQ(server.lastRequestBody(), QByteArray("future targeted auto send"));
}

TEST(CloudClipboardControllerTest, ManualSendDoesNotDuplicateMatchingActiveAutomaticSend) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    MockHttpServer::Route delayed = json(R"({"payload_id":"auto","recipient_count":1})");
    delayed.delayMs = 200;
    const QString route = QStringLiteral("/v1/clipboard/send-targeted?target=device-2");
    server.setRoute(route, delayed);
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("target"), {}, true, false}});
    controller.setSelectedTargetDeviceId(QStringLiteral("device-2"));
    controller.setAutoSendEnabled(true);

    QApplication::clipboard()->setText(QStringLiteral("one automatic delivery"));
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(route), 1, 5000);
    FC_TRY_VERIFY_WITH_TIMEOUT(!controller.items().isEmpty(), 1000);
    controller.sendRecord(controller.items().first().id, QStringLiteral("device-2"));
    QTest::qWait(350);

    EXPECT_EQ(server.requestCount(route), 1);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 0);
}

TEST(CloudClipboardControllerTest, StaleAutoSendTargetNeverFallsBackToAllDevices) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("target"), {}, false, false}});
    controller.setSelectedTargetDeviceId(QStringLiteral("device-2"));
    controller.setAutoSendEnabled(true);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this"), {}, true, true}});
    EXPECT_FALSE(controller.targetDeviceIsAvailable());

    QApplication::clipboard()->setText(QStringLiteral("must not broadcast"));
    FC_TRY_VERIFY_WITH_TIMEOUT(!controller.items().isEmpty(), 1000);
    QTest::qWait(100);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 0);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send-targeted?target=device-2")), 0);
}

TEST(CloudClipboardControllerTest, AutoSendNeverResendsIncomingOrControllerCopy) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord incoming = store.addIncomingText(QStringLiteral("incoming"),
                                                                   QStringLiteral("device-2"), QStringLiteral("peer"));
    ASSERT_FALSE(incoming.id.isEmpty());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    server.setRoute(QStringLiteral("/v1/clipboard/send"), json(R"({"payload_id":"all","recipient_count":1})"));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("peer"), {}, false, false}});
    controller.setAutoSendEnabled(true);
    ASSERT_TRUE(controller.copyRecordToClipboard(incoming.id));
    QCoreApplication::processEvents();
    QTest::qWait(100);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 0);
}

TEST(CloudClipboardControllerTest, DisablingAutoSendDropsPendingAutomaticWorkButKeepsManualWork) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord manual = store.addLocalText(QStringLiteral("manual first"));
    ASSERT_FALSE(manual.id.isEmpty());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    MockHttpServer::Route delayed = json(R"({"payload_id":"one","recipient_count":1})");
    delayed.delayMs = 250;
    server.setRouteSequence(QStringLiteral("/v1/clipboard/send"),
                            {delayed, json(R"({"payload_id":"two","recipient_count":1})")});
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("peer"), {}, false, false}});
    controller.sendRecord(manual.id);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1, 5000);
    controller.setAutoSendEnabled(true);
    QApplication::clipboard()->setText(QStringLiteral("automatic queued then disabled"));
    FC_TRY_VERIFY_WITH_TIMEOUT(controller.items().size() >= 2, 1000);
    controller.setAutoSendEnabled(false);
    QTest::qWait(400);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1);
}

TEST(CloudClipboardControllerTest, AgentAnnouncementRefreshesMissedDeliveries) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"), json(R"({"deliveries":[]})"));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    DeviceAgent agent(&client);
    CloudClipboardController controller(store, &client, &agent);

    signIn(client, server);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries")), 1,
                                5000);

    ASSERT_TRUE(QMetaObject::invokeMethod(&agent, "announced", Qt::DirectConnection));
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries")), 2,
                                5000);
}

TEST(CloudClipboardControllerTest, CopyingARecordDoesNotRecaptureItsOwnClipboardWrite) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord added = store.addLocalText(QStringLiteral("copy once"));
    ASSERT_FALSE(added.id.isEmpty());
    CloudClipboardController controller(store, nullptr);
    QSignalSpy changed(&controller, &CloudClipboardController::changed);

    ASSERT_TRUE(controller.copyRecordToClipboard(added.id));
    QCoreApplication::processEvents();

    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("copy once"));
    EXPECT_EQ(controller.items().size(), 1);
    EXPECT_TRUE(changed.isEmpty());
}

TEST(CloudClipboardControllerTest, SendsOnlySelectedLocalRecordExplicitly) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord local = store.addLocalText(QStringLiteral("send explicitly"));
    ASSERT_FALSE(local.id.isEmpty());

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    server.setRoute(QStringLiteral("/v1/clipboard/send"),
                    json(R"({"payload_id":"payload-1","recipient_count":1})"));
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("other device"), {}, false, false}});
    QSignalSpy finished(&client, &AccountClient::clipboardSendFinished);

    controller.sendRecord(local.id);
    FC_TRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);

    EXPECT_EQ(server.lastRequestBody(), QByteArray("send explicitly"));
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard")), 0);
    EXPECT_EQ(controller.items().size(), 1);

    ClipboardHistoryRecord incoming = store.addIncomingText(QStringLiteral("never resend"),
                                                             QStringLiteral("device-2"),
                                                             QStringLiteral("other device"));
    ASSERT_FALSE(incoming.id.isEmpty());
    controller.sendRecord(incoming.id);
    QCoreApplication::processEvents();
    EXPECT_EQ(finished.count(), 1);
}

TEST(CloudClipboardControllerTest, BatchSendSerializesLocalRecordsAndSkipsIncoming) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("batch first"));
    const ClipboardHistoryRecord incoming = store.addIncomingText(
        QStringLiteral("batch incoming"), QStringLiteral("device-2"), QStringLiteral("other device"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("batch second"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(incoming.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    MockHttpServer::Route delayed = json(R"({"payload_id":"payload-1","recipient_count":1})");
    delayed.delayMs = 150;
    server.setRouteSequence(QStringLiteral("/v1/clipboard/send"),
                            {delayed, json(R"({"payload_id":"payload-2","recipient_count":1})")});
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("other device"), {}, false, false}});
    QSignalSpy finished(&controller, &CloudClipboardController::transferFinished);
    QSignalSpy status(&controller, &CloudClipboardController::transferStatusChanged);

    controller.sendRecords({first.id, incoming.id, second.id});
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1, 5000);
    QTest::qWait(30);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1);
    FC_TRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);

    ASSERT_EQ(finished.at(0).at(0).toString(), first.id);
    ASSERT_EQ(finished.at(1).at(0).toString(), second.id);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 2);
    EXPECT_EQ(server.lastRequestBody(), QByteArray("batch second"));
    ASSERT_FALSE(status.isEmpty());
    EXPECT_EQ(status.last().at(0).toString(), QStringLiteral("Queued 2 selected records."));
}

TEST(CloudClipboardControllerTest, BatchSendContinuesAfterFailure) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("failing batch item"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("successful batch item"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    server.setRouteSequence(QStringLiteral("/v1/clipboard/send"),
                            {json(R"({"detail":"send failed"})", 500),
                             json(R"({"payload_id":"payload-2","recipient_count":1})")});
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("other device"), {}, false, false}});
    QSignalSpy finished(&controller, &CloudClipboardController::transferFinished);
    QSignalSpy status(&controller, &CloudClipboardController::transferStatusChanged);

    controller.sendRecords({first.id, second.id});
    FC_TRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);

    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 2);
    EXPECT_EQ(server.lastRequestBody(), QByteArray("successful batch item"));
    ASSERT_FALSE(status.isEmpty());
    EXPECT_EQ(status.last().at(0).toString(), QStringLiteral("Queued 1 of 2 records; 1 failed."));
}

TEST(CloudClipboardControllerTest, MissingQueuedRecordFinishesBatchStatus) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("active batch item"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("evicted queued item"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    MockHttpServer::Route delayed = json(R"({"payload_id":"payload-1","recipient_count":1})");
    delayed.delayMs = 150;
    server.setRouteSequence(QStringLiteral("/v1/clipboard/send"),
                            {delayed, json(R"({"payload_id":"payload-2","recipient_count":1})")});
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("other device"), {}, false, false}});
    QSignalSpy finished(&controller, &CloudClipboardController::transferFinished);
    QSignalSpy status(&controller, &CloudClipboardController::transferStatusChanged);

    controller.sendRecords({first.id, second.id});
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1, 5000);
    ASSERT_TRUE(store.remove(second.id));
    FC_TRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    FC_TRY_VERIFY_WITH_TIMEOUT(!status.isEmpty() &&
                                   status.last().at(0).toString() ==
                                       QStringLiteral("Queued 1 selected records."),
                               5000);

    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1);
}

TEST(CloudClipboardControllerTest, TextRefreshFailureReleasesActiveSendForTheNextRecord) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("first refresh failure"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("second after refresh"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    server.setRoute(QStringLiteral("/v1/clipboard/send"), json(R"({"detail":"expired"})", 401));
    server.setRoute(QStringLiteral("/v1/auth/refresh"), json(R"({"detail":"refresh refused"})", 500));
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("other device"), {}, false, false}});
    QSignalSpy failed(&client, &AccountClient::clipboardSendFailed);

    controller.sendRecord(first.id);
    FC_TRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);

    signIn(client, server);
    server.setRoute(QStringLiteral("/v1/clipboard/send"),
                    json(R"({"payload_id":"payload-2","recipient_count":1})"));
    QSignalSpy sent(&client, &AccountClient::clipboardSendFinished);
    controller.sendRecord(second.id);
    FC_TRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, 5000);
    EXPECT_EQ(server.lastRequestBody(), QByteArray("second after refresh"));
}

TEST(CloudClipboardControllerTest, CompletesEveryDeliveryEvenWithRepeatedTerminalStatus) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const QString firstId = QStringLiteral("delivery-first");
    const QString secondId = QStringLiteral("delivery-second");
    const QByteArray firstContent("first overlapping delivery");
    const QByteArray secondContent("second overlapping delivery");
    const auto delivery = [](const QString &id, const QByteArray &content) {
        const QByteArray hash = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
        return QByteArray("{\"id\":\"") + id.toUtf8() + "\",\"payload_id\":\"payload-" +
               id.toUtf8() + "\",\"kind\":\"text\",\"mime\":\"text/plain\",\"size\":" +
               QByteArray::number(content.size()) + ",\"sha256\":\"" + hash +
               "\",\"source_device_id\":\"device-2\",\"source_device_name\":\"other device\",\"created\":\"2026-08-23T12:00:00Z\"}";
    };

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(QByteArray("{\"deliveries\":[") + delivery(firstId, firstContent) + "," +
                         delivery(secondId, secondContent) + "]}"));
    for (const auto &entry : {std::pair{firstId, firstContent}, std::pair{secondId, secondContent}}) {
        MockHttpServer::Route content;
        content.contentType = "text/plain; charset=utf-8";
        content.body = entry.second;
        content.delayMs = 30;
        content.headers.insert("X-Content-Sha256",
                               QCryptographicHash::hash(entry.second, QCryptographicHash::Sha256).toHex());
        server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + entry.first +
                        QStringLiteral("/content"), content);
        server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + entry.first +
                        QStringLiteral("/ack"), json({}, 204));
    }

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    QSignalSpy status(&controller, &CloudClipboardController::transferStatusChanged);
    QSignalSpy finished(&controller, &CloudClipboardController::transferFinished);
    signIn(client, server);

    FC_TRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
    EXPECT_EQ(store.records().size(), 2);
    const QString completedFirst = finished.at(0).at(0).toString();
    const QString completedSecond = finished.at(1).at(0).toString();
    EXPECT_NE(completedFirst, completedSecond);
    EXPECT_TRUE(completedFirst == firstId || completedFirst == secondId);
    EXPECT_TRUE(completedSecond == firstId || completedSecond == secondId);

    int receivedStatuses = 0;
    for (const QList<QVariant> &arguments : status) {
        if (arguments.at(0).toString() == QStringLiteral("Delivery received."))
            ++receivedStatuses;
    }
    EXPECT_EQ(receivedStatuses, 1);
}

TEST(CloudClipboardControllerTest, IncomingTextIsStoredAcknowledgedAndNeverOverwritesClipboard) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const QByteArray content("incoming delivery");
    const QByteArray hash = QCryptographicHash::hash(content, QCryptographicHash::Sha256);

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json("{\"deliveries\":[{\"id\":\"delivery-1\",\"payload_id\":\"payload-1\",\"kind\":\"text\",\"mime\":\"text/plain\",\"size\":17,\"sha256\":\"" + hash.toHex() + "\",\"source_device_id\":\"device-2\",\"source_device_name\":\"other device\",\"created\":\"2026-08-23T12:00:00Z\"}]}"));
    MockHttpServer::Route contentRoute;
    contentRoute.contentType = "text/plain; charset=utf-8";
    contentRoute.body = content;
    contentRoute.headers.insert("X-Content-Sha256", hash.toHex());
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/delivery-1/content"), contentRoute);
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/delivery-1/ack"), json({}, 204));

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    QApplication::clipboard()->setText(QStringLiteral("keep local clipboard"));
    signIn(client, server);

    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/delivery-1/ack")),
                                1, 5000);
    ASSERT_FALSE(controller.items().isEmpty());
    const ClipboardHistoryRecord &record = controller.items().first();
    EXPECT_EQ(record.origin, ClipboardRecordOrigin::Incoming);
    EXPECT_EQ(record.text, QStringLiteral("incoming delivery"));
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("keep local clipboard"));
}

TEST(CloudClipboardControllerTest, ExternalIdenticalClipboardChangeIsCapturedAfterSynchronousCopy) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord same = store.addLocalText(QStringLiteral("identical external text"));
    ASSERT_FALSE(same.id.isEmpty());
    CloudClipboardController controller(store, nullptr);
    QSignalSpy changed(&controller, &CloudClipboardController::changed);

    ASSERT_TRUE(controller.copyRecordToClipboard(same.id));
    QApplication::clipboard()->setText(QStringLiteral("intervening external text"));
    FC_TRY_COMPARE_WITH_TIMEOUT(controller.items().size(), 2, 1000);
    changed.clear();
    QApplication::clipboard()->setText(QStringLiteral("identical external text"));
    FC_TRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 1000);

    EXPECT_EQ(controller.items().first().id, same.id);
}

TEST(CloudClipboardControllerTest, ConcurrentRefreshDoesNotRedownloadDeliveryAwaitingAcknowledgement) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const QByteArray content("pending acknowledgement");

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(deliveryListJson(QStringLiteral("delivery-race"), QStringLiteral("text"), content)));
    MockHttpServer::Route contentRoute;
    contentRoute.contentType = "text/plain; charset=utf-8";
    contentRoute.body = content;
    contentRoute.headers.insert("X-Content-Sha256", QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/delivery-race/content"), contentRoute);
    MockHttpServer::Route ack = json({}, 204);
    ack.delayMs = 300;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/delivery-race/ack"), ack);

    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    signIn(client, server);
    FC_TRY_COMPARE_WITH_TIMEOUT(store.records().size(), 1, 5000);
    controller.refresh();
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries")), 2, 5000);
    QTest::qWait(80);

    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/delivery-race/content")), 1);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/delivery-race/ack")), 1);
    EXPECT_EQ(store.records().size(), 1);
}

TEST(CloudClipboardControllerTest, SerializesOverlappingSendsUnderTheFirstRecordIdentity) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("first send"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("second send"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    MockHttpServer server;
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    signIn(client, server);
    MockHttpServer::Route pending;
    pending.silent = true;
    server.setRoute(QStringLiteral("/v1/clipboard/send"), pending);
    CloudClipboardController controller(store, &client);
    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("device-2"), QStringLiteral("other device"), {}, false, false}});
    QSignalSpy progress(&controller, &CloudClipboardController::transferProgress);

    controller.sendRecord(first.id);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1, 5000);
    controller.sendRecord(second.id);
    QTest::qWait(30);
    QMetaObject::invokeMethod(&client, "clipboardSendProgress", Qt::DirectConnection,
                              Q_ARG(qint64, 4), Q_ARG(qint64, 10));

    ASSERT_EQ(progress.count(), 1);
    EXPECT_EQ(progress.first().at(0).toString(), first.id);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/send")), 1);
}

TEST(CloudClipboardControllerTest, GenericRequestFailureDoesNotForgetOtherInFlightDelivery) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const QByteArray content("in flight content");
    const QString id = QStringLiteral("delivery-in-flight");

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(deliveryListJson(id, QStringLiteral("text"), content)));
    MockHttpServer::Route contentRoute;
    contentRoute.contentType = "text/plain; charset=utf-8";
    contentRoute.body = content;
    contentRoute.delayMs = 300;
    contentRoute.headers.insert("X-Content-Sha256", QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content"), contentRoute);
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack"), json({}, 204));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    signIn(client, server);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content")), 1, 5000);

    QMetaObject::invokeMethod(&client, "requestFailed", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("unrelated request failed")));
    controller.refresh();
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries")), 2, 5000);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content")), 1);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack")), 1, 5000);
}

TEST(CloudClipboardControllerTest, RemoveAndClearEmitDeselection) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("selected first"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("selected second"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());
    CloudClipboardController controller(store, nullptr);
    QSignalSpy selected(&controller, &CloudClipboardController::selectionChanged);

    controller.selectRecord(first.id);
    selected.clear();
    ASSERT_TRUE(controller.removeRecord(first.id));
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.first().first().toString(), second.id);
    EXPECT_EQ(controller.selectedRecordId(), second.id);

    selected.clear();
    controller.clear();
    ASSERT_EQ(selected.count(), 1);
    EXPECT_TRUE(selected.first().first().toString().isEmpty());
}

TEST(CloudClipboardControllerTest, BulkRemovalUsesPreferredVisibleSuccessor) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("first"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("second"));
    const ClipboardHistoryRecord third = store.addLocalText(QStringLiteral("third"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());
    ASSERT_FALSE(third.id.isEmpty());

    CloudClipboardController controller(store, nullptr);
    controller.selectRecord(second.id);
    QSignalSpy selected(&controller, &CloudClipboardController::selectionChanged);
    QSignalSpy changed(&controller, &CloudClipboardController::changed);
    selected.clear();
    changed.clear();

    ASSERT_TRUE(controller.removeRecords({second.id}, first.id));
    ASSERT_FALSE(store.lookup(second.id));
    EXPECT_EQ(controller.selectedRecordId(), first.id);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.first().first().toString(), first.id);
    EXPECT_EQ(changed.count(), 1);

    selected.clear();
    changed.clear();
    ASSERT_TRUE(controller.removeRecords({first.id, third.id}));
    EXPECT_TRUE(controller.selectedRecordId().isEmpty());
    ASSERT_EQ(selected.count(), 1);
    EXPECT_TRUE(selected.first().first().toString().isEmpty());
    EXPECT_EQ(changed.count(), 1);
}

TEST(CloudClipboardControllerTest, FailedAcknowledgementRetriesWithoutRedownloadingOrDuplicatingHistory) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const QByteArray content("retry acknowledgement");
    const QString id = QStringLiteral("delivery-ack-retry");

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(deliveryListJson(id, QStringLiteral("text"), content)));
    MockHttpServer::Route contentRoute;
    contentRoute.contentType = "text/plain; charset=utf-8";
    contentRoute.body = content;
    contentRoute.headers.insert("X-Content-Sha256", QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content"), contentRoute);
    server.setRouteSequence(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack"),
                            {json(R"({"detail":"temporary failure"})", 500), json({}, 204)});
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    signIn(client, server);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack")), 1, 5000);
    ASSERT_EQ(store.records().size(), 1);

    controller.refresh();
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack")), 2, 5000);
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content")), 1);
    EXPECT_EQ(store.records().size(), 1);
}

TEST(CloudClipboardControllerTest, IncomingImagePersistsThenAcknowledgesAndCleansStagingFile) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    QImage image;
    const QByteArray content = pngBytes(&image);
    const QString id = QStringLiteral("delivery-image");

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(deliveryListJson(id, QStringLiteral("image"), content, image)));
    serveDeliveryContent(server, id, content);
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack"), json({}, 204));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    QSignalSpy downloaded(&client, &AccountClient::clipboardDownloadFinished);
    signIn(client, server);

    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack")), 1, 5000);
    ASSERT_EQ(downloaded.count(), 1);
    EXPECT_FALSE(QFile::exists(downloaded.first().at(1).toString()));
    ASSERT_EQ(store.records().size(), 1);
    const ClipboardHistoryRecord &record = store.records().first();
    EXPECT_EQ(record.origin, ClipboardRecordOrigin::Incoming);
    EXPECT_EQ(record.kind, ClipboardRecordKind::Image);
    EXPECT_TRUE(QFile::exists(record.imagePath));
}

TEST(CloudClipboardControllerTest, InvalidIncomingImageIsNotAcknowledgedAndCanRetry) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    const QByteArray invalid("not an image");
    const QString id = QStringLiteral("delivery-invalid-image");
    QImage declared(1, 1, QImage::Format_ARGB32);
    declared.fill(Qt::black);

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(deliveryListJson(id, QStringLiteral("image"), invalid, declared)));
    serveDeliveryContent(server, id, invalid);
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack"), json({}, 204));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    QSignalSpy downloaded(&client, &AccountClient::clipboardDownloadFinished);
    signIn(client, server);

    FC_TRY_COMPARE_WITH_TIMEOUT(downloaded.count(), 1, 5000);
    EXPECT_FALSE(QFile::exists(downloaded.first().at(1).toString()));
    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack")), 0);
    EXPECT_TRUE(store.records().isEmpty());
    controller.refresh();
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content")), 2, 5000);
    FC_TRY_COMPARE_WITH_TIMEOUT(downloaded.count(), 2, 5000);
}

TEST(CloudClipboardControllerTest, ImageAcknowledgementRefreshFailureRetriesWithoutRedownloadingHistory) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    QImage image;
    const QByteArray content = pngBytes(&image);
    const QString id = QStringLiteral("delivery-image-refresh");

    MockHttpServer server;
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries"),
                    json(deliveryListJson(id, QStringLiteral("image"), content, image)));
    serveDeliveryContent(server, id, content);
    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack"),
                    json(R"({"detail":"expired"})", 401));
    server.setRoute(QStringLiteral("/v1/auth/refresh"), json(R"({"detail":"refresh refused"})", 500));
    AccountClient client;
    client.setApiUrl(server.url(QString()));
    CloudClipboardController controller(store, &client);
    QSignalSpy ackFailed(&client, &AccountClient::clipboardDeliveryAcknowledgementFailed);
    signIn(client, server);
    FC_TRY_COMPARE_WITH_TIMEOUT(ackFailed.count(), 1, 5000);
    ASSERT_EQ(store.records().size(), 1);

    server.setRoute(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack"), json({}, 204));
    signIn(client, server);
    FC_TRY_COMPARE_WITH_TIMEOUT(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/ack")), 2, 5000);

    EXPECT_EQ(server.requestCount(QStringLiteral("/v1/clipboard/deliveries/") + id + QStringLiteral("/content")), 1);
    EXPECT_EQ(store.records().size(), 1);
}

TEST(CloudClipboardControllerTest, LogoutResetsTransferPresentation) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ClipboardHistoryStore store(directory.path());
    ASSERT_FALSE(store.addLocalText(QStringLiteral("local survives logout")).id.isEmpty());
    ASSERT_FALSE(store.addIncomingText(QStringLiteral("incoming removed on logout"),
                                       QStringLiteral("device-a"), QStringLiteral("A")).id.isEmpty());
    AccountClient client;
    CloudClipboardController controller(store, &client);
    QSignalSpy reset(&controller, &CloudClipboardController::transferReset);

    ASSERT_TRUE(QMetaObject::invokeMethod(&client, "loggedOut", Qt::DirectConnection));

    ASSERT_EQ(reset.count(), 1);
    EXPECT_EQ(controller.state(), CloudClipboardController::State::SignedOut);
    ASSERT_EQ(store.records().size(), 1);
    EXPECT_EQ(store.records().first().origin, ClipboardRecordOrigin::Local);
}

TEST(CloudClipboardPanelTest, ShowsOnlyReadOnlyHistoryPreviewControls) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    NotepadPanel panel(settings);

    auto *status = panel.findChild<QLabel *>(QStringLiteral("CloudClipboardStatus"));
    auto *preview = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardPreview"));
    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardEditor"));
    auto *upload = panel.findChild<QCheckBox *>(QStringLiteral("CloudClipboardAutoUpload"));
    auto *receive = panel.findChild<QCheckBox *>(QStringLiteral("CloudClipboardAutoReceive"));
    ASSERT_NE(status, nullptr);
    ASSERT_NE(preview, nullptr);
    EXPECT_TRUE(preview->isReadOnly());
    EXPECT_EQ(editor, nullptr);
    EXPECT_EQ(upload, nullptr);
    EXPECT_EQ(receive, nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("Sign in")));
}

TEST(CloudClipboardPanelTest, SelectionPreviewsTextAndFooterActionsTrackSelection) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord local = store.addLocalText(QStringLiteral("panel local"));
    const ClipboardHistoryRecord incoming = store.addIncomingText(QStringLiteral("panel incoming"),
                                                                   QStringLiteral("device-2"),
                                                                   QStringLiteral("other device"));
    ASSERT_FALSE(local.id.isEmpty());
    ASSERT_FALSE(incoming.id.isEmpty());
    CloudClipboardController controller(store, nullptr);
    controller.setDevices({{QStringLiteral("self"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("online"), QStringLiteral("Desk"), {}, true, false},
                           {QStringLiteral("offline"), QStringLiteral("Laptop"), {}, false, false}});
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *preview = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardPreview"));
    auto *copy = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardCopyButton"));
    auto *autoSend = panel.findChild<QCheckBox *>(QStringLiteral("CloudClipboardAutoSend"));
    auto *target = panel.findChild<QComboBox *>(QStringLiteral("CloudClipboardTargetDeviceCombo"));
    auto *send = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardSendButton"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(autoSend, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(send, nullptr);
    EXPECT_EQ(send->text(), QStringLiteral("Send"));
    ASSERT_EQ(target->count(), 3);
    EXPECT_EQ(target->itemText(0), QStringLiteral("All devices"));
    EXPECT_TRUE(target->itemData(0).toString().isEmpty());
    EXPECT_EQ(target->findData(QStringLiteral("self")), -1);
    const int onlineIndex = target->findData(QStringLiteral("online"));
    const int offlineIndex = target->findData(QStringLiteral("offline"));
    ASSERT_GE(onlineIndex, 0);
    ASSERT_GE(offlineIndex, 0);
    EXPECT_TRUE(target->itemText(onlineIndex).contains(QStringLiteral("Online")));
    EXPECT_TRUE(target->itemText(offlineIndex).contains(QStringLiteral("Offline")));

    panel.show();
    QCoreApplication::processEvents();

    auto select = [list](const QString &id) {
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toString() == id) {
                list->setCurrentRow(row);
                return;
            }
        }
        ADD_FAILURE() << "history record missing from list";
    };
    select(local.id);
    QCoreApplication::processEvents();
    EXPECT_EQ(preview->toPlainText(), local.text);
    EXPECT_TRUE(send->isVisible());
    EXPECT_TRUE(send->isEnabled());
    EXPECT_TRUE(copy->isEnabled());
    QApplication::clipboard()->setText(QStringLiteral("before footer copy"));
    QTest::mouseClick(copy, Qt::LeftButton);
    QCoreApplication::processEvents();
    EXPECT_EQ(QApplication::clipboard()->text(), local.text);
    EXPECT_FALSE(autoSend->isChecked());
    QTest::mouseClick(autoSend, Qt::LeftButton);
    EXPECT_TRUE(controller.autoSendEnabled());
    target->setCurrentIndex(offlineIndex);
    QCoreApplication::processEvents();
    EXPECT_EQ(controller.selectedTargetDeviceId(), QStringLiteral("offline"));
    EXPECT_EQ(send->focusPolicy(), Qt::StrongFocus);
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toString() == incoming.id)
            list->item(row)->setSelected(true);
    }
    QCoreApplication::processEvents();
    EXPECT_TRUE(send->isEnabled());
    preview->setFocus();
    ASSERT_TRUE(preview->hasFocus());
    QTest::keyClick(preview, Qt::Key_Tab);
    EXPECT_TRUE(copy->hasFocus());
    QTest::keyClick(copy, Qt::Key_Tab);
    EXPECT_TRUE(autoSend->hasFocus());
    QTest::keyClick(autoSend, Qt::Key_Tab);
    EXPECT_TRUE(target->hasFocus());
    QTest::keyClick(target, Qt::Key_Tab);
    EXPECT_TRUE(send->hasFocus());

    list->clearSelection();
    select(incoming.id);
    QCoreApplication::processEvents();
    EXPECT_EQ(preview->toPlainText(), incoming.text);
    EXPECT_TRUE(send->isVisible());
    EXPECT_FALSE(send->isEnabled());
}

TEST(CloudClipboardPanelTest, UnavailableTargetNeverFallsBackToAllDevices) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    settings.setCloudClipboardTargetDeviceId(QStringLiteral("removed-device"));
    CloudClipboardController controller(settings, nullptr);
    controller.setDevices({{QStringLiteral("self"), QStringLiteral("this device"), {}, true, true},
                           {QStringLiteral("other"), QStringLiteral("Other"), {}, true, false}});
    NotepadPanel panel(settings, &controller);
    auto *target = panel.findChild<QComboBox *>(QStringLiteral("CloudClipboardTargetDeviceCombo"));
    auto *send = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardSendButton"));
    ASSERT_NE(target, nullptr);
    ASSERT_NE(send, nullptr);

    const int unavailableIndex = target->findData(QStringLiteral("removed-device"));
    ASSERT_GE(unavailableIndex, 0);
    EXPECT_EQ(target->itemText(unavailableIndex), QStringLiteral("Unavailable device"));
    EXPECT_EQ(target->currentIndex(), unavailableIndex);
    EXPECT_FALSE(controller.targetDeviceIsAvailable());
    EXPECT_FALSE(send->isEnabled());

    target->setCurrentIndex(0);
    QCoreApplication::processEvents();
    EXPECT_TRUE(controller.selectedTargetDeviceId().isEmpty());
    EXPECT_TRUE(controller.targetDeviceIsAvailable());
}

TEST(CloudClipboardPanelTest, DeleteSelectedRecordKeepsStableIdAndClearsSelection) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord record = store.addLocalText(QStringLiteral("delete without crash"));
    ASSERT_FALSE(record.id.isEmpty());
    const QString recordId = record.id;

    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *deleteButton = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardDeleteButton"));
    auto *preview = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardPreview"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_EQ(list->count(), 1);

    QSignalSpy selectionChanged(&controller, &CloudClipboardController::selectionChanged);
    panel.show();
    QCoreApplication::processEvents();
    list->setCurrentRow(0);
    QCoreApplication::processEvents();
    ASSERT_EQ(controller.selectedRecordId(), recordId);
    ASSERT_TRUE(deleteButton->isEnabled());
    ASSERT_EQ(preview->toPlainText(), record.text);
    selectionChanged.clear();

    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    EXPECT_FALSE(store.lookup(recordId));
    EXPECT_TRUE(controller.selectedRecordId().isEmpty());
    ASSERT_EQ(selectionChanged.count(), 1);
    EXPECT_TRUE(selectionChanged.first().first().toString().isEmpty());
    EXPECT_EQ(list->count(), 0);
    EXPECT_FALSE(deleteButton->isEnabled());
    EXPECT_TRUE(preview->toPlainText().isEmpty());
}

TEST(CloudClipboardPanelTest, ExtendedSelectionRowsAndBatchDeleteKeepNextVisibleRecordFocused) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("first"));
    const ClipboardHistoryRecord middle = store.addIncomingText(QStringLiteral("middle"),
                                                                  QStringLiteral("device-2"),
                                                                  QStringLiteral("other device"));
    const ClipboardHistoryRecord last = store.addLocalText(QStringLiteral("last"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(middle.id.isEmpty());
    ASSERT_FALSE(last.id.isEmpty());

    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *deleteButton = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardDeleteButton"));
    auto *preview = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardPreview"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_EQ(list->selectionMode(), QAbstractItemView::ExtendedSelection);
    ASSERT_NE(dynamic_cast<CloudClipboardRowDelegate *>(list->itemDelegate()), nullptr);
    ASSERT_EQ(list->count(), 3);
    EXPECT_EQ(list->item(0)->sizeHint().height(), list->item(1)->sizeHint().height());
    EXPECT_EQ(list->item(1)->sizeHint().height(), list->item(2)->sizeHint().height());
    EXPECT_EQ(list->item(0)->sizeHint().height(), 44);
    EXPECT_EQ(list->item(0)->data(CloudClipboardRowDelegate::KindRole).toString(), QStringLiteral("text"));
    EXPECT_FALSE(list->item(0)->data(CloudClipboardRowDelegate::TimeRole).toString().isEmpty());
    EXPECT_FALSE(list->item(0)->data(CloudClipboardRowDelegate::DeviceRole).toString().isEmpty());

    panel.show();
    QCoreApplication::processEvents();
    list->setFocus();
    list->item(0)->setSelected(true);
    list->item(1)->setSelected(true);
    list->setCurrentItem(list->item(1), QItemSelectionModel::NoUpdate);
    QCoreApplication::processEvents();
    ASSERT_TRUE(deleteButton->isEnabled());

    QTest::keyClick(list, Qt::Key_Delete);
    QCoreApplication::processEvents();

    EXPECT_FALSE(store.lookup(last.id));
    EXPECT_FALSE(store.lookup(middle.id));
    ASSERT_TRUE(store.lookup(first.id));
    ASSERT_EQ(list->count(), 1);
    ASSERT_NE(list->currentItem(), nullptr);
    EXPECT_EQ(list->currentItem()->data(CloudClipboardRowDelegate::RecordIdRole).toString(), first.id);
    EXPECT_TRUE(list->currentItem()->isSelected());
    EXPECT_EQ(controller.selectedRecordId(), first.id);
    EXPECT_EQ(preview->toPlainText(), first.text);
    EXPECT_TRUE(list->hasFocus());
}

TEST(CloudClipboardPanelTest, TrailingDeleteFallsBackToPreviousVisibleRecord) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("first trailing"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("second trailing"));
    const ClipboardHistoryRecord third = store.addLocalText(QStringLiteral("third trailing"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());
    ASSERT_FALSE(third.id.isEmpty());

    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *deleteButton = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardDeleteButton"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_EQ(list->count(), 3);

    panel.show();
    QCoreApplication::processEvents();
    list->setCurrentRow(2);
    QCoreApplication::processEvents();
    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    EXPECT_FALSE(store.lookup(first.id));
    ASSERT_NE(list->currentItem(), nullptr);
    EXPECT_EQ(list->currentItem()->data(CloudClipboardRowDelegate::RecordIdRole).toString(), second.id);
    EXPECT_EQ(controller.selectedRecordId(), second.id);
    EXPECT_TRUE(list->currentItem()->isSelected());
}

TEST(CloudClipboardPanelTest, DataRebuildPreservesExtendedSelectionAndCurrentRecord) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("preserve first"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("preserve second"));
    const ClipboardHistoryRecord third = store.addLocalText(QStringLiteral("preserve third"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());
    ASSERT_FALSE(third.id.isEmpty());

    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 3);

    list->item(0)->setSelected(true);
    list->item(2)->setSelected(true);
    list->setCurrentItem(list->item(0), QItemSelectionModel::NoUpdate);
    QCoreApplication::processEvents();
    const QString currentId = list->currentItem()->data(CloudClipboardRowDelegate::RecordIdRole).toString();
    QSet<QString> selectedBefore;
    for (QListWidgetItem *item : list->selectedItems())
        selectedBefore.insert(item->data(CloudClipboardRowDelegate::RecordIdRole).toString());
    ASSERT_EQ(selectedBefore.size(), 2);

    controller.setDevices({{QStringLiteral("device-1"), QStringLiteral("this device"), {}, true, true}});
    QCoreApplication::processEvents();

    ASSERT_NE(list->currentItem(), nullptr);
    EXPECT_EQ(list->currentItem()->data(CloudClipboardRowDelegate::RecordIdRole).toString(), currentId);
    QSet<QString> selectedAfter;
    for (QListWidgetItem *item : list->selectedItems())
        selectedAfter.insert(item->data(CloudClipboardRowDelegate::RecordIdRole).toString());
    EXPECT_EQ(selectedAfter, selectedBefore);
}

TEST(CloudClipboardPanelTest, FilteredBatchDeleteUsesNextMatchingRecord) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord hidden = store.addLocalText(QStringLiteral("hidden"));
    const ClipboardHistoryRecord first = store.addLocalText(QStringLiteral("match first"));
    const ClipboardHistoryRecord second = store.addLocalText(QStringLiteral("match second"));
    const ClipboardHistoryRecord third = store.addLocalText(QStringLiteral("match third"));

    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *search = panel.findChild<QLineEdit *>(QStringLiteral("CloudClipboardSearch"));
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *deleteButton = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardDeleteButton"));
    ASSERT_NE(search, nullptr);
    ASSERT_NE(list, nullptr);
    ASSERT_NE(deleteButton, nullptr);
    search->setText(QStringLiteral("match"));
    QCoreApplication::processEvents();
    ASSERT_EQ(list->count(), 3);

    list->setCurrentRow(1);
    QCoreApplication::processEvents();
    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    EXPECT_FALSE(store.lookup(second.id));
    ASSERT_NE(list->currentItem(), nullptr);
    EXPECT_EQ(list->currentItem()->data(CloudClipboardRowDelegate::RecordIdRole).toString(), first.id);
    EXPECT_EQ(controller.selectedRecordId(), first.id);
    EXPECT_TRUE(store.lookup(hidden.id));
    EXPECT_TRUE(store.lookup(third.id));
}

TEST(CloudClipboardPanelTest, ImageSelectionPreviewsScaledImagesAndHandlesMissingFiles) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());

    auto encode = [](const QImage &image) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        EXPECT_TRUE(buffer.open(QIODevice::WriteOnly));
        EXPECT_TRUE(image.save(&buffer, "PNG"));
        return bytes;
    };
    QImage source(840, 440, QImage::Format_ARGB32);
    source.fill(Qt::green);
    const ClipboardHistoryRecord valid = store.addLocalImage(encode(source), QStringLiteral("image/png"),
                                                              source.width(), source.height());
    QImage unavailable(7, 5, QImage::Format_ARGB32);
    unavailable.fill(Qt::red);
    const ClipboardHistoryRecord missing = store.addLocalImage(encode(unavailable), QStringLiteral("image/png"),
                                                                unavailable.width(), unavailable.height());
    ASSERT_FALSE(valid.id.isEmpty());
    ASSERT_FALSE(missing.id.isEmpty());
    ASSERT_TRUE(QFile::remove(missing.imagePath));

    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *stack = panel.findChild<QStackedWidget *>(QStringLiteral("CloudClipboardContentStack"));
    auto *imagePreview = panel.findChild<QLabel *>(QStringLiteral("CloudClipboardImagePreview"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(stack, nullptr);
    ASSERT_NE(imagePreview, nullptr);

    auto select = [list](const QString &id) {
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toString() == id) {
                list->setCurrentRow(row);
                return;
            }
        }
        ADD_FAILURE() << "history image missing from list";
    };
    select(valid.id);
    EXPECT_EQ(stack->currentWidget(), imagePreview);
    const QPixmap *pixmap = imagePreview->pixmap();
    ASSERT_NE(pixmap, nullptr);
    EXPECT_FALSE(pixmap->isNull());
    EXPECT_EQ(pixmap->size(), QSize(420, 220));
    EXPECT_TRUE(imagePreview->text().isEmpty());

    select(missing.id);
    EXPECT_EQ(stack->currentWidget(), imagePreview);
    const QPixmap *missingPixmap = imagePreview->pixmap();
    EXPECT_TRUE(missingPixmap == nullptr || missingPixmap->isNull());
    EXPECT_EQ(imagePreview->text(), QStringLiteral("Could not load image preview."));
}

TEST(CloudClipboardPanelTest, DoubleClickCopiesSelectedRecordWithoutAutomaticIncomingCopy) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    const ClipboardHistoryRecord incoming = store.addIncomingText(QStringLiteral("copy on double click"),
                                                                   QStringLiteral("device-2"),
                                                                   QStringLiteral("other device"));
    ASSERT_FALSE(incoming.id.isEmpty());
    QApplication::clipboard()->setText(QStringLiteral("keep until double click"));
    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("keep until double click"));

    panel.show();
    QCoreApplication::processEvents();
    list->setCurrentItem(list->item(0));
    ASSERT_TRUE(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                                          Q_ARG(QListWidgetItem *, list->item(0))));
    QCoreApplication::processEvents();
    EXPECT_EQ(QApplication::clipboard()->text(), incoming.text);
}

TEST(CloudClipboardPanelTest, TransferSignalsUpdateProgressAndStatus) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    ClipboardHistoryStore store(temporaryDir.path());
    CloudClipboardController controller(store, nullptr);
    NotepadPanel panel(settings, &controller);
    auto *progress = panel.findChild<QProgressBar *>(QStringLiteral("CloudClipboardProgress"));
    auto *status = panel.findChild<QLabel *>(QStringLiteral("CloudClipboardStatus"));
    ASSERT_NE(progress, nullptr);
    ASSERT_NE(status, nullptr);
    panel.show();
    QCoreApplication::processEvents();

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferProgress", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("record-1")),
                                          Q_ARG(qint64, 25), Q_ARG(qint64, 100)));
    EXPECT_TRUE(progress->isVisible());
    EXPECT_EQ(progress->value(), 25);
    EXPECT_TRUE(status->text().contains(QStringLiteral("25%")));
    EXPECT_TRUE(status->text().contains(QStringLiteral("25 / 100 bytes")));

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferProgress", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("record-2")),
                                          Q_ARG(qint64, 75), Q_ARG(qint64, 100)));
    EXPECT_EQ(progress->value(), 50);
    EXPECT_TRUE(status->text().contains(QStringLiteral("100 / 200 bytes")));

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferStatusChanged", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("Delivery received."))));
    EXPECT_TRUE(progress->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferFinished", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("record-1"))));
    EXPECT_TRUE(progress->isVisible());
    EXPECT_EQ(progress->value(), 75);
    EXPECT_TRUE(status->text().contains(QStringLiteral("75 / 100 bytes")));

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferProgress", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("record-2")),
                                          Q_ARG(qint64, 50), Q_ARG(qint64, 200)));
    EXPECT_TRUE(progress->isVisible());
    EXPECT_EQ(progress->value(), 25);
    EXPECT_TRUE(status->text().contains(QStringLiteral("50 / 200 bytes")));

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferStatusChanged", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("Delivery received."))));
    EXPECT_TRUE(progress->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferFinished", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("record-2"))));
    EXPECT_FALSE(progress->isVisible());
    EXPECT_EQ(progress->value(), 0);
    EXPECT_EQ(status->text(), QStringLiteral("Delivery received."));

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferProgress", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("record-3")),
                                          Q_ARG(qint64, 10), Q_ARG(qint64, 100)));
    EXPECT_TRUE(progress->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferReset", Qt::DirectConnection));
    EXPECT_FALSE(progress->isVisible());
    EXPECT_EQ(progress->value(), 0);
}

TEST(CloudClipboardPanelTest, PreservesAnchoredPopupGeometryWithPreview) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    NotepadPanel panel(settings);
    const QRect appContent(100, 100, 700, 700);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    EXPECT_EQ(panel.geometry().top(), appContent.top());
    EXPECT_EQ(panel.geometry().bottom() + 1, anchor.top());
    auto *preview = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardPreview"));
    ASSERT_NE(preview, nullptr);
    EXPECT_GE(preview->height(), 100);
}
