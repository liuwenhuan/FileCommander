#include <gtest/gtest.h>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QUrl>

#include "MockHttpServer.h"
#include "TryUntil.h"
#include "CloudClipboardController.h"
#include "NotepadPanel.h"
#include "account/AccountClient.h"
#include "account/ClipboardHistoryStore.h"
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
    CloudClipboardController controller(store, nullptr);
    QSignalSpy changed(&controller, &CloudClipboardController::changed);
    const QString text = QStringLiteral("local capture %1").arg(QUuid::createUuid().toString());

    QApplication::clipboard()->setText(text);
    FC_TRY_COMPARE_WITH_TIMEOUT(controller.items().size(), 1, 1000);

    const ClipboardHistoryRecord &record = controller.items().first();
    EXPECT_EQ(record.origin, ClipboardRecordOrigin::Local);
    EXPECT_EQ(record.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(record.text, text);
    EXPECT_TRUE(changed.count() > 0);
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
    EXPECT_TRUE(selected.first().first().toString().isEmpty());

    controller.selectRecord(second.id);
    selected.clear();
    controller.clear();
    ASSERT_EQ(selected.count(), 1);
    EXPECT_TRUE(selected.first().first().toString().isEmpty());
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

TEST(CloudClipboardPanelTest, SelectionPreviewsTextAndShowsSendOnlyForLocalRecord) {
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
    NotepadPanel panel(settings, &controller);
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("CloudClipboardList"));
    auto *preview = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardPreview"));
    auto *send = panel.findChild<QPushButton *>(QStringLiteral("CloudClipboardSendButton"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(send, nullptr);

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

    select(incoming.id);
    QCoreApplication::processEvents();
    EXPECT_EQ(preview->toPlainText(), incoming.text);
    EXPECT_FALSE(send->isVisible());
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

    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "transferStatusChanged", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("Delivery received."))));
    EXPECT_EQ(status->text(), QStringLiteral("Delivery received."));
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
