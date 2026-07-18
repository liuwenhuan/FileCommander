#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FileInfo.h"

TEST(FileInfoTest, ReadsRegularFileMetadata) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = dir.filePath("hello.txt");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello world");
    file.close();

    FileInfo info(path);
    EXPECT_EQ(info.name().toStdString(), "hello.txt");
    EXPECT_EQ(info.suffix().toStdString(), "txt");
    EXPECT_EQ(info.size(), 11);
    EXPECT_FALSE(info.isDir());
    EXPECT_FALSE(info.isParentEntry());
    EXPECT_TRUE(info.isValid());
}

TEST(FileInfoTest, ReadsDirectoryMetadata) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("subdir"));

    FileInfo info(dir.filePath("subdir"));
    EXPECT_EQ(info.name().toStdString(), "subdir");
    EXPECT_TRUE(info.isDir());
    EXPECT_TRUE(info.suffix().isEmpty());
}

TEST(FileInfoTest, PermissionsStringHasTenCharacters) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("perm.txt");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    FileInfo info(path);
    EXPECT_EQ(info.permissionsString().size(), 10);
}

TEST(FileInfoTest, DefaultConstructedIsInvalid) {
    FileInfo info;
    EXPECT_FALSE(info.isValid());
}
