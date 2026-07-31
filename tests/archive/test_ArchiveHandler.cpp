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

TEST(ArchiveHandlerTest, BuildTreeOpensZipFromUnicodeFilesystemPath) {
    QTemporaryDir srcDir, dir;
    ASSERT_TRUE(srcDir.isValid() && dir.isValid());
    const QString filePath = writeFile(srcDir.path(), "a.txt", "hello unicode zip");
    const QString sourceArchive = QDir(dir.path()).filePath(QStringLiteral("ascii.zip"));
    QString createErr;
    ASSERT_TRUE(ArchiveHandler::create(sourceArchive, {filePath}, QStringLiteral("zip"),
                                       &createErr))
        << createErr.toStdString();
    const QString unicodeArchive = QDir(dir.path()).filePath(QStringLiteral("语音输入法交互设计.zip"));
    ASSERT_TRUE(QFile::copy(sourceArchive, unicodeArchive));

    QString err;
    auto root = ArchiveHandler::buildTree(unicodeArchive, &err);
    ASSERT_TRUE(root) << err.toStdString();
    EXPECT_TRUE(root->findChild("a.txt"));
}

TEST(ArchiveHandlerTest, EnvArchivePathBuildsTree) {
    const QString archivePath = QString::fromLocal8Bit(qgetenv("FILECOMMANDER_ARCHIVE_TEST_PATH"));
    if (archivePath.isEmpty()) {
        GTEST_SKIP() << "FILECOMMANDER_ARCHIVE_TEST_PATH is not set";
    }
    ASSERT_TRUE(QFile::exists(archivePath)) << archivePath.toStdString();

    QString err;
    auto root = ArchiveHandler::buildTree(archivePath, &err);
    ASSERT_TRUE(root) << err.toStdString();
    EXPECT_FALSE(root->children.isEmpty());
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

TEST(ArchiveHandlerTest, CreateZipRoundTripsThroughExtract) {
    QTemporaryDir srcDir, workDir, destDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid() && destDir.isValid());
    const QString filePath = writeFile(srcDir.path(), "greeting.txt", "hello zip");
    writeFile(srcDir.path(), "folder/deep.txt", "deep content");

    const QString archivePath = QDir(workDir.path()).filePath("out.zip");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {filePath, QDir(srcDir.path()).filePath("folder")},
                                        "zip", &err))
        << err.toStdString();
    ASSERT_TRUE(QFile::exists(archivePath));

    auto root = ArchiveHandler::buildTree(archivePath, &err);
    ASSERT_TRUE(root) << err.toStdString();
    EXPECT_TRUE(root->findChild("greeting.txt"));
    auto folder = root->findChild("folder");
    ASSERT_TRUE(folder);
    EXPECT_TRUE(folder->findChild("deep.txt"));

    ASSERT_TRUE(ArchiveHandler::extract(archivePath, {}, destDir.path(), &err))
        << err.toStdString();
    QFile extracted(QDir(destDir.path()).filePath("greeting.txt"));
    ASSERT_TRUE(extracted.open(QIODevice::ReadOnly));
    EXPECT_EQ(extracted.readAll(), QByteArray("hello zip"));
    EXPECT_TRUE(QFile::exists(QDir(destDir.path()).filePath("folder/deep.txt")));
}

TEST(ArchiveHandlerTest, CreateTarGzRoundTripsThroughExtract) {
    QTemporaryDir srcDir, workDir, destDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid() && destDir.isValid());
    const QString filePath = writeFile(srcDir.path(), "note.txt", "hello targz");

    const QString archivePath = QDir(workDir.path()).filePath("out.tar.gz");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {filePath}, "tar.gz", &err))
        << err.toStdString();

    ASSERT_TRUE(ArchiveHandler::extract(archivePath, {}, destDir.path(), &err))
        << err.toStdString();
    QFile extracted(QDir(destDir.path()).filePath("note.txt"));
    ASSERT_TRUE(extracted.open(QIODevice::ReadOnly));
    EXPECT_EQ(extracted.readAll(), QByteArray("hello targz"));
}

TEST(ArchiveHandlerTest, BuildTreeFailsGracefullyOnMissingFile) {
    QString err;
    auto root = ArchiveHandler::buildTree("/nonexistent/path/does_not_exist.tar.gz", &err);
    EXPECT_FALSE(root);
    EXPECT_FALSE(err.isEmpty());
}
