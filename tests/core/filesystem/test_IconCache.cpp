#include <gtest/gtest.h>

#include <QFile>
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
