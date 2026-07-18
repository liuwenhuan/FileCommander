#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include "ArchiveHandler.h"

namespace {

QString writeFile(const QString &dir, const QString &relPath, const QByteArray &content) {
    const QString path = QDir(dir).filePath(relPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
    return path;
}

// Builds source/{a.txt, sub/b.txt} then tars it into archiveDir/test.tar.gz
// via the system `tar` binary, returning the archive path.
QString makeTestArchive(const QString &archiveDir) {
    QTemporaryDir srcDir;
    writeFile(srcDir.path(), "a.txt", "hello a");
    writeFile(srcDir.path(), "sub/b.txt", "hello b");

    const QString archivePath = QDir(archiveDir).filePath("test.tar.gz");
    QProcess proc;
    proc.setWorkingDirectory(srcDir.path());
    proc.start("tar", {"-czf", archivePath, "a.txt", "sub"});
    proc.waitForFinished(10000);
    return archivePath;
}

} // namespace

TEST(ArchiveHandlerTest, IsSupportedArchiveRecognizesKnownExtensions) {
    EXPECT_TRUE(ArchiveHandler::isSupportedArchive("foo.zip"));
    EXPECT_TRUE(ArchiveHandler::isSupportedArchive("foo.tar.gz"));
    EXPECT_TRUE(ArchiveHandler::isSupportedArchive("FOO.TAR.BZ2"));
    EXPECT_FALSE(ArchiveHandler::isSupportedArchive("foo.txt"));
}

TEST(ArchiveHandlerTest, BuildTreeParsesFilesAndDirectories) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString archivePath = makeTestArchive(dir.path());
    ASSERT_TRUE(QFile::exists(archivePath));

    QString err;
    auto root = ArchiveHandler::buildTree(archivePath, &err);
    ASSERT_TRUE(root) << err.toStdString();

    auto a = root->findChild("a.txt");
    ASSERT_TRUE(a);
    EXPECT_FALSE(a->isDir);
    EXPECT_EQ(a->size, 7); // "hello a"

    auto sub = root->findChild("sub");
    ASSERT_TRUE(sub);
    EXPECT_TRUE(sub->isDir);
    auto b = sub->findChild("b.txt");
    ASSERT_TRUE(b);
    EXPECT_FALSE(b->isDir);
    EXPECT_EQ(b->fullPath, QString("sub/b.txt"));
}

TEST(ArchiveHandlerTest, ExtractAllWritesAllFiles) {
    QTemporaryDir dir, destDir;
    ASSERT_TRUE(dir.isValid() && destDir.isValid());
    const QString archivePath = makeTestArchive(dir.path());

    QString err;
    ASSERT_TRUE(ArchiveHandler::extract(archivePath, {}, destDir.path(), &err))
        << err.toStdString();

    QFile a(QDir(destDir.path()).filePath("a.txt"));
    ASSERT_TRUE(a.open(QIODevice::ReadOnly));
    EXPECT_EQ(a.readAll(), QByteArray("hello a"));

    EXPECT_TRUE(QFile::exists(QDir(destDir.path()).filePath("sub/b.txt")));
}

TEST(ArchiveHandlerTest, ExtractSpecificEntryWritesOnlyThatFile) {
    QTemporaryDir dir, destDir;
    ASSERT_TRUE(dir.isValid() && destDir.isValid());
    const QString archivePath = makeTestArchive(dir.path());

    QString err;
    ASSERT_TRUE(ArchiveHandler::extract(archivePath, {"a.txt"}, destDir.path(), &err))
        << err.toStdString();

    EXPECT_TRUE(QFile::exists(QDir(destDir.path()).filePath("a.txt")));
    EXPECT_FALSE(QFile::exists(QDir(destDir.path()).filePath("sub/b.txt")));
}

TEST(ArchiveHandlerTest, BuildTreeFailsGracefullyOnMissingFile) {
    QString err;
    auto root = ArchiveHandler::buildTree("/nonexistent/path/does_not_exist.tar.gz", &err);
    EXPECT_FALSE(root);
    EXPECT_FALSE(err.isEmpty());
}
