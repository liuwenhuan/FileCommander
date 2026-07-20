#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FileInfo.h"

TEST(FileInfoTest, MimeTypeIsComputedLazilyAndCorrectly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("note.txt");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("plain text content");
    file.close();

    // Deferred (not computed in the constructor), then resolved on first
    // access and stable on subsequent calls.
    FileInfo info(path);
    const QString mime = info.mimeType();
    EXPECT_EQ(mime.toStdString(), "text/plain");
    EXPECT_EQ(info.mimeType().toStdString(), "text/plain");
}

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
