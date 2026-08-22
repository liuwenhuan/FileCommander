#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include "account/CloudClipboardImageCache.h"

namespace {

const QString kItemId = QStringLiteral("3b20b7ee-aeca-4f1e-a950-401880dfd9f7");

TEST(CloudClipboardImageCacheTest, StoresAndOpensTheOriginalBytesWithMetadata) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CloudClipboardImageCache cache(dir.filePath(QStringLiteral("images")));
    const QByteArray original("\x89PNG\r\noriginal-image-bytes", 26);

    ASSERT_TRUE(cache.store(kItemId, original, QStringLiteral("image/png")));

    CloudClipboardImageCache::Entry entry;
    ASSERT_TRUE(cache.lookup(kItemId, &entry));
    EXPECT_EQ(entry.itemId, kItemId);
    EXPECT_EQ(entry.mimeType, QStringLiteral("image/png"));
    EXPECT_EQ(entry.size, original.size());
    EXPECT_EQ(entry.sha256.size(), 32);
    EXPECT_GT(entry.expiresAt, QDateTime::currentDateTimeUtc());
    EXPECT_FALSE(QFileInfo(entry.filePath).isSymbolicLink());

    std::unique_ptr<QFile> file = cache.openRead(kItemId);
    ASSERT_TRUE(file);
    EXPECT_EQ(file->readAll(), original);
}

TEST(CloudClipboardImageCacheTest, RejectsUnsafeIdsOversizedImagesAndUnsafeMimeTypes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CloudClipboardImageCache cache(dir.filePath(QStringLiteral("images")));

    EXPECT_FALSE(CloudClipboardImageCache::isSafeItemId(QStringLiteral("../secret")));
    EXPECT_FALSE(CloudClipboardImageCache::isSafeItemId(QStringLiteral("{3b20b7ee-aeca-4f1e-a950-401880dfd9f7}")));
    EXPECT_FALSE(cache.store(QStringLiteral("../secret"), "image", QStringLiteral("image/png")));
    EXPECT_FALSE(cache.store(kItemId, "image", QStringLiteral("image/png\r\nX-Injected: yes")));
    EXPECT_FALSE(cache.store(kItemId,
                             QByteArray(CloudClipboardImageCache::maximumImageBytes + 1, 'x'),
                             QStringLiteral("image/png")));
}

TEST(CloudClipboardImageCacheTest, ExpiresEntriesAndCleansThemUp) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CloudClipboardImageCache cache(dir.filePath(QStringLiteral("images")));
    ASSERT_TRUE(cache.store(kItemId, "short-lived", QStringLiteral("image/png"),
                            QDateTime::currentDateTimeUtc().addSecs(1)));

    QThread::msleep(1200);
    EXPECT_FALSE(cache.lookup(kItemId));
    cache.cleanup();
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("images/")) + kItemId +
                                   QStringLiteral(".bin")));
}

TEST(CloudClipboardImageCacheTest, RefusesTamperedEntriesAndCleansThemUp) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CloudClipboardImageCache cache(dir.filePath(QStringLiteral("images")));
    const QByteArray original("unmodified original");
    ASSERT_TRUE(cache.store(kItemId, original, QStringLiteral("image/jpeg")));

    const QString dataPath = dir.filePath(QStringLiteral("images/")) + kItemId + QStringLiteral(".bin");
    QFile data(dataPath);
    ASSERT_TRUE(data.open(QIODevice::WriteOnly | QIODevice::Append));
    ASSERT_EQ(data.write("x"), 1);
    data.close();

    EXPECT_FALSE(cache.lookup(kItemId));
    cache.cleanup();
    EXPECT_FALSE(QFileInfo::exists(dataPath));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("images/")) + kItemId +
                                   QStringLiteral(".json")));
}

} // namespace
