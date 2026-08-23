#include <gtest/gtest.h>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QImage>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
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

TEST(CloudClipboardPanelTest, StartsSignedOutWithAutomaticSyncOff) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    NotepadPanel panel(settings);

    auto *status = panel.findChild<QLabel *>(QStringLiteral("CloudClipboardStatus"));
    auto *upload = panel.findChild<QCheckBox *>(QStringLiteral("CloudClipboardAutoUpload"));
    auto *receive = panel.findChild<QCheckBox *>(QStringLiteral("CloudClipboardAutoReceive"));
    ASSERT_NE(status, nullptr);
    ASSERT_NE(upload, nullptr);
    ASSERT_NE(receive, nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("Sign in")));
    EXPECT_FALSE(upload->isChecked());
    EXPECT_FALSE(receive->isEnabled());
    EXPECT_FALSE(settings.cloudClipboardAutoUpload());
    EXPECT_FALSE(settings.cloudClipboardAutoReceive());
}

TEST(CloudClipboardPanelTest, PreservesAnchoredPopupGeometryAndEditorHeight) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    settings.setCloudClipboardEditorHeight(130);
    NotepadPanel panel(settings);
    const QRect appContent(100, 100, 700, 700);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    EXPECT_EQ(panel.geometry().top(), appContent.top());
    EXPECT_EQ(panel.geometry().bottom() + 1, anchor.top());
    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardEditor"));
    ASSERT_NE(editor, nullptr);
    EXPECT_GE(editor->height(), 100);
}
