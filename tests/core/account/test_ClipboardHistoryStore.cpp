#include <gtest/gtest.h>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>

#include "account/ClipboardHistoryStore.h"

namespace {

TEST(ClipboardHistoryStoreTest, ImageWinsOverHtmlAndTextFallback) {
    QMimeData mime;
    mime.setImageData(QImage(8, 8, QImage::Format_ARGB32));
    mime.setHtml(QStringLiteral("<b>fallback</b>"));
    mime.setText(QStringLiteral("fallback"));

    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Image);
}

TEST(ClipboardHistoryStoreTest, RichTextStoresPlainTextOnly) {
    QMimeData mime;
    mime.setHtml(QStringLiteral("<b>Hello</b>"));
    mime.setText(QStringLiteral("Hello"));

    ClipboardCapture capture;
    ASSERT_TRUE(ClipboardHistoryStore::captureFromMimeData(&mime, &capture));
    EXPECT_EQ(capture.kind, ClipboardRecordKind::Text);
    EXPECT_EQ(capture.text, QStringLiteral("Hello"));
}

TEST(ClipboardHistoryStoreTest, RejectsFilesAndPrivateFormats) {
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/secret.txt"))});
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&files, nullptr));

    QMimeData privateMime;
    privateMime.setData("application/x-password", "secret");
    privateMime.setText(QStringLiteral("secret"));
    EXPECT_FALSE(ClipboardHistoryStore::captureFromMimeData(&privateMime, nullptr));
}

QByteArray encodedPng(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (buffer.open(QIODevice::WriteOnly))
        image.save(&buffer, "PNG");
    return bytes;
}

QString historyPath(const QTemporaryDir &dir) {
    return dir.filePath(QStringLiteral("cloud-clipboard/history"));
}

TEST(ClipboardHistoryStoreTest, PersistsFiftyNewestAndDeduplicates) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ClipboardHistoryStore store(dir.path());
    for (int i = 0; i < 51; ++i)
        ASSERT_FALSE(store.addLocalText(QString::number(i)).id.isEmpty());
    ASSERT_FALSE(store.addLocalText(QStringLiteral("50")).id.isEmpty());

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 50);
    EXPECT_EQ(reloaded.records().first().text, QStringLiteral("50"));
    EXPECT_EQ(std::count_if(reloaded.records().cbegin(), reloaded.records().cend(),
                            [](const ClipboardHistoryRecord &record) {
                                return record.text == QLatin1String("50");
                            }),
              1);
    EXPECT_TRUE(reloaded.records().back().text != QLatin1String("0"));
}

TEST(ClipboardHistoryStoreTest, ReencodesImagesAndPersistsTheirPrivateRelativeReference) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(8, 6, QImage::Format_ARGB32);
    image.fill(Qt::red);

    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord added = store.addLocalImage(encodedPng(image),
                                                              QStringLiteral("image/jpeg"), 8, 6);
    ASSERT_FALSE(added.id.isEmpty());
    EXPECT_EQ(added.kind, ClipboardRecordKind::Image);
    EXPECT_EQ(added.mime, QStringLiteral("image/png"));
    EXPECT_TRUE(QFileInfo::exists(added.imagePath));

    QFile imageFile(added.imagePath);
    ASSERT_TRUE(imageFile.open(QIODevice::ReadOnly));
    const QByteArray stored = imageFile.readAll();
    EXPECT_EQ(added.size, stored.size());
    EXPECT_EQ(added.sha256, QString::fromLatin1(
                                QCryptographicHash::hash(stored, QCryptographicHash::Sha256).toHex()));
    QImage decoded;
    ASSERT_TRUE(decoded.loadFromData(stored));
    EXPECT_EQ(decoded.size(), image.size());

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 1);
    EXPECT_EQ(reloaded.records().first().id, added.id);
    EXPECT_EQ(reloaded.records().first().imagePath, added.imagePath);

    QFile manifest(QDir(historyPath(dir)).filePath(QStringLiteral("manifest.json")));
    ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
    EXPECT_FALSE(manifest.readAll().contains(dir.path().toUtf8()));
}

TEST(ClipboardHistoryStoreTest, QuarantinesCorruptManifestAndRebuildsEmptyHistory) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(historyPath(dir)));
    QFile manifest(QDir(historyPath(dir)).filePath(QStringLiteral("manifest.json")));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    ASSERT_EQ(manifest.write("not json"), 8);
    manifest.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_TRUE(store.records().isEmpty());
    EXPECT_TRUE(QFileInfo::exists(manifest.fileName()));
    EXPECT_TRUE(QFileInfo::exists(QDir(historyPath(dir)).filePath(
        QStringLiteral("manifest.json.corrupt"))));
}

TEST(ClipboardHistoryStoreTest, RemovesOrphanedImageFilesDuringLoad) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString images = QDir(historyPath(dir)).filePath(QStringLiteral("images"));
    ASSERT_TRUE(QDir().mkpath(images));
    const QString orphan = QDir(images).filePath(QStringLiteral("orphan.bin"));
    QFile file(orphan);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("orphan"), 6);
    file.close();

    ClipboardHistoryStore store(dir.path());
    ASSERT_TRUE(store.load());
    EXPECT_FALSE(QFileInfo::exists(orphan));
}

TEST(ClipboardHistoryStoreTest, RemovesRecordsAndTheirImageFiles) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord added = store.addLocalImage(encodedPng(image),
                                                              QStringLiteral("image/png"), 2, 2);
    ASSERT_FALSE(added.id.isEmpty());

    ClipboardHistoryRecord found;
    ASSERT_TRUE(store.lookup(added.id, &found));
    EXPECT_EQ(found.imagePath, added.imagePath);
    ASSERT_TRUE(store.remove(added.id));
    EXPECT_FALSE(store.lookup(added.id, nullptr));
    EXPECT_FALSE(QFileInfo::exists(added.imagePath));
}

TEST(ClipboardHistoryStoreTest, PreservesIncomingProvenanceAfterPersistentStorage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QDateTime deliveredAt = QDateTime::fromString(QStringLiteral("2026-08-23T12:00:00Z"), Qt::ISODate);
    ClipboardHistoryStore store(dir.path());
    const ClipboardHistoryRecord added = store.addIncomingText(
        QStringLiteral("from another device"), QStringLiteral("device-2"), QStringLiteral("Laptop"),
        deliveredAt);
    ASSERT_FALSE(added.id.isEmpty());
    EXPECT_EQ(added.origin, ClipboardRecordOrigin::Incoming);
    EXPECT_EQ(added.sourceDeviceId, QStringLiteral("device-2"));
    EXPECT_EQ(added.sourceDeviceName, QStringLiteral("Laptop"));
    EXPECT_EQ(added.created, deliveredAt);

    ClipboardHistoryStore reloaded(dir.path());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.records().size(), 1);
    EXPECT_EQ(reloaded.records().first().origin, ClipboardRecordOrigin::Incoming);
    EXPECT_EQ(reloaded.records().first().sourceDeviceId, QStringLiteral("device-2"));
}

} // namespace
