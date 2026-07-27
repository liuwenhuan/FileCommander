#include <gtest/gtest.h>

#include <QColor>
#include <QFile>
#include <QPixmap>
#include <QTemporaryDir>

#include "FileInfo.h"
#include "IconCache.h"

TEST(IconCacheTest, ReturnsNonNullIconForFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("a.txt");
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.close();

    FileInfo info(path);
    QIcon icon = IconCache::instance().iconFor(info);
    EXPECT_FALSE(icon.isNull());
}

TEST(IconCacheTest, ReturnsNonNullIconForDirectory) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FileInfo info(dir.path());
    QIcon icon = IconCache::instance().iconFor(info);
    EXPECT_FALSE(icon.isNull());
}

TEST(IconCacheTest, ThemedIconLeavesOriginalUntouchedWithoutTint) {
    QPixmap source(16, 16);
    source.fill(QColor(130, 80, 40, 173));
    const QIcon raw(source);

    IconCache::instance().setTint(QColor());
    const QIcon result = IconCache::instance().themedIcon(raw);

    EXPECT_EQ(result.cacheKey(), raw.cacheKey());
}

TEST(IconCacheTest, ThemedIconUsesConfiguredTintAndLeavesAlphaIntact) {
    QPixmap source(16, 16);
    source.fill(QColor(130, 80, 40, 173));
    const QIcon raw(source);

    IconCache::instance().setTint(QColor(0, 255, 0), 0);
    const QPixmap tinted = IconCache::instance().themedIcon(raw).pixmap(source.size());
    IconCache::instance().setTint(QColor());

    ASSERT_FALSE(tinted.isNull());
    const QColor pixel = tinted.toImage().pixelColor(0, 0);
    EXPECT_EQ(pixel.red(), 0);
    EXPECT_GT(pixel.green(), 0);
    EXPECT_EQ(pixel.blue(), 0);
    EXPECT_EQ(pixel.alpha(), 173);
}

TEST(IconCacheTest, SameExtensionReusesCachedIcon) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QFile a(dir.filePath("a.txt"));
    a.open(QIODevice::WriteOnly);
    a.close();
    QFile b(dir.filePath("b.txt"));
    b.open(QIODevice::WriteOnly);
    b.close();

    QIcon iconA = IconCache::instance().iconFor(FileInfo(dir.filePath("a.txt")));
    QIcon iconB = IconCache::instance().iconFor(FileInfo(dir.filePath("b.txt")));
    // Both .txt files should resolve through the same cache entry.
    EXPECT_FALSE(iconA.isNull());
    EXPECT_FALSE(iconB.isNull());
    EXPECT_EQ(iconA.cacheKey(), iconB.cacheKey());
}
