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
    EXPECT_EQ(info.baseName().toStdString(), "hello");
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
    EXPECT_EQ(info.baseName().toStdString(), "subdir"); // dirs: whole name is the base
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

TEST(FileInfoTest, FromFieldsSplitsFileNameAndKeepsMetadata) {
    const QDateTime mtime = QDateTime::fromSecsSinceEpoch(1600000000);
    const FileInfo info = FileInfo::fromFields(
        "/remote/dir/photo.jpg", "photo.jpg", 2048, mtime, /*isDir=*/false,
        QFile::ReadOwner | QFile::WriteOwner);
    EXPECT_EQ(info.name().toStdString(), "photo.jpg");
    EXPECT_EQ(info.baseName().toStdString(), "photo");
    EXPECT_EQ(info.suffix().toStdString(), "jpg");
    EXPECT_EQ(info.path().toStdString(), "/remote/dir/photo.jpg");
    EXPECT_EQ(info.size(), 2048);
    EXPECT_EQ(info.modified(), mtime);
    EXPECT_FALSE(info.isDir());
    EXPECT_FALSE(info.isSymLink());
    EXPECT_TRUE(info.isValid());
    EXPECT_FALSE(info.created().isValid()); // SFTP has no creation time
}

TEST(FileInfoTest, HiddenNamesOnlyUseADotAfterTheLeadingDotAsExtension) {
    const FileInfo hidden = FileInfo::fromFields(
        "/remote/.bashrc", ".bashrc", 0, QDateTime(), /*isDir=*/false, QFile::ReadOwner);
    EXPECT_EQ(hidden.baseName(), QStringLiteral(".bashrc"));
    EXPECT_TRUE(hidden.suffix().isEmpty());

    const FileInfo dottedHidden = FileInfo::fromFields(
        "/remote/.config.json", ".config.json", 0, QDateTime(), /*isDir=*/false,
        QFile::ReadOwner);
    EXPECT_EQ(dottedHidden.baseName(), QStringLiteral(".config"));
    EXPECT_EQ(dottedHidden.suffix(), QStringLiteral("json"));

    const FileInfo localHidden("/tmp/.bashrc");
    EXPECT_EQ(localHidden.baseName(), QStringLiteral(".bashrc"));
    EXPECT_TRUE(localHidden.suffix().isEmpty());
}

TEST(FileInfoTest, FromFieldsTreatsDirectoryNameAsWholeBase) {
    const FileInfo info = FileInfo::fromFields(
        "/remote/my.data", "my.data", 0, QDateTime(), /*isDir=*/true, QFile::ReadOwner);
    EXPECT_EQ(info.baseName().toStdString(), "my.data"); // dirs: no suffix split
    EXPECT_TRUE(info.suffix().isEmpty());
    EXPECT_TRUE(info.isDir());
}
