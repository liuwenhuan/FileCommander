#include <gtest/gtest.h>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QImage>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QUrl>

#include "CloudClipboardController.h"
#include "NotepadPanel.h"
#include "config/Settings.h"

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

    EXPECT_EQ(panel.geometry().bottom() + 1, anchor.top());
    EXPECT_GE(panel.geometry().top(), appContent.top());
    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("CloudClipboardEditor"));
    ASSERT_NE(editor, nullptr);
    EXPECT_GE(editor->height(), 100);
}
