#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QTemporaryDir>
#include <QVector>

#include "ArchiveHandler.h"
#include "ArchiveLayout.h"
#include "ArchiveProvider.h"
#include "FileInfo.h"

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

// Reads a whole entry's bytes back out through the provider's streaming API.
QByteArray readEntry(ArchiveProvider &provider, const QString &virtualPath) {
    FileHandle *h = provider.openRead(virtualPath);
    if (!h)
        return QByteArray();
    QByteArray out;
    char buf[4096];
    qint64 n;
    while ((n = provider.read(h, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<int>(n));
    provider.closeHandle(h);
    return out;
}

bool listHasName(const QVector<FileInfo> &entries, const QString &name) {
    for (const FileInfo &fi : entries)
        if (fi.name() == name)
            return true;
    return false;
}

} // namespace

// --- ArchiveLayout (Deliverable 1) -----------------------------------------

TEST(ArchiveLayoutTest, StripsSingleTopLevelFolder) {
    const QStringList entries = {"root/", "root/a.txt", "root/sub/b.txt"};
    const auto r = ArchiveLayout::analyze(entries, "pkg.zip");
    EXPECT_TRUE(r.stripSingleRoot);
    EXPECT_EQ(r.strippedPrefix, QString("root"));
    EXPECT_FALSE(r.wrapInArchiveNamedFolder);
}

TEST(ArchiveLayoutTest, WrapsMultipleTopLevelItems) {
    const QStringList entries = {"a.txt", "sub/b.txt"};
    const auto r = ArchiveLayout::analyze(entries, "pkg.zip");
    EXPECT_FALSE(r.stripSingleRoot);
    EXPECT_TRUE(r.wrapInArchiveNamedFolder);
}

TEST(ArchiveLayoutTest, SingleFileAtRootIsNeitherStrippedNorWrapped) {
    const QStringList entries = {"only.txt"};
    const auto r = ArchiveLayout::analyze(entries, "pkg.zip");
    EXPECT_FALSE(r.stripSingleRoot);
    EXPECT_FALSE(r.wrapInArchiveNamedFolder);
}

TEST(ArchiveLayoutTest, DetectsNestedArchiveAfterStrip) {
    const QStringList entries = {"root/", "root/inner.tar.gz"};
    const auto r = ArchiveLayout::analyze(entries, "outer.zip");
    EXPECT_TRUE(r.stripSingleRoot);
    EXPECT_TRUE(r.resultIsSingleArchive);
    EXPECT_EQ(r.innerArchiveName, QString("inner.tar.gz"));
}

TEST(ArchiveLayoutTest, DetectsSingleArchiveFileAtRoot) {
    const QStringList entries = {"payload.7z"};
    const auto r = ArchiveLayout::analyze(entries, "outer.zip");
    EXPECT_TRUE(r.resultIsSingleArchive);
    EXPECT_EQ(r.innerArchiveName, QString("payload.7z"));
}

// --- ArchiveProvider (Deliverable 2) ---------------------------------------

// zip with a single root folder -> strip + per-entry extraction path.
TEST(ArchiveProviderTest, ZipSingleRootStrippedAndReadsBytes) {
    QTemporaryDir srcDir, workDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid());
    // Build source/pkg/{greeting.txt, nested/deep.txt}
    writeFile(srcDir.path(), "pkg/greeting.txt", "hello zip");
    writeFile(srcDir.path(), "pkg/nested/deep.txt", "deep content");

    const QString archivePath = QDir(workDir.path()).filePath("bundle.zip");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {QDir(srcDir.path()).filePath("pkg")}, "zip",
                                       &err))
        << err.toStdString();

    ArchiveProvider provider(archivePath, &err);
    ASSERT_TRUE(provider.isValid()) << err.toStdString();

    // (a) The single root "pkg" was stripped: its contents appear at "/".
    const QVector<FileInfo> rootList = provider.list("/", true);
    EXPECT_TRUE(listHasName(rootList, "greeting.txt"));
    EXPECT_TRUE(listHasName(rootList, "nested"));
    EXPECT_FALSE(listHasName(rootList, "pkg")); // the wrapper is gone

    // (b) Directory navigation works on virtual paths.
    EXPECT_TRUE(provider.isDir("/nested"));
    EXPECT_TRUE(provider.exists("/greeting.txt"));
    const QVector<FileInfo> nestedList = provider.list("/nested", true);
    EXPECT_TRUE(listHasName(nestedList, "deep.txt"));

    // (c) Reading a known entry returns its exact bytes.
    EXPECT_EQ(readEntry(provider, "/greeting.txt"), QByteArray("hello zip"));
    EXPECT_EQ(readEntry(provider, "/nested/deep.txt"), QByteArray("deep content"));
}

// tar.gz (non-zip) -> extract-all-on-first-read path, with progress callback.
TEST(ArchiveProviderTest, TarGzExtractAllOnFirstReadWithProgress) {
    QTemporaryDir srcDir, workDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid());
    writeFile(srcDir.path(), "top/one.txt", "first file");
    writeFile(srcDir.path(), "top/two.txt", "second file");

    const QString archivePath = QDir(workDir.path()).filePath("bundle.tar.gz");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {QDir(srcDir.path()).filePath("top")},
                                       "tar.gz", &err))
        << err.toStdString();

    ArchiveProvider provider(archivePath, &err);
    ASSERT_TRUE(provider.isValid()) << err.toStdString();

    qint64 lastDone = -1, lastTotal = -1;
    provider.setProgressCallback([&](qint64 done, qint64 total) {
        lastDone = done;
        lastTotal = total;
    });

    const QVector<FileInfo> rootList = provider.list("/", true);
    EXPECT_TRUE(listHasName(rootList, "one.txt"));
    EXPECT_TRUE(listHasName(rootList, "two.txt"));

    EXPECT_EQ(readEntry(provider, "/one.txt"), QByteArray("first file"));
    // Second read served from the already-extracted temp dir.
    EXPECT_EQ(readEntry(provider, "/two.txt"), QByteArray("second file"));

    // Progress fired and reached the total (sum of file sizes).
    EXPECT_GE(lastDone, 0);
    EXPECT_EQ(lastDone, lastTotal);
}

TEST(ArchiveProviderTest, IsArchivePathRecognizesSuffixes) {
    EXPECT_TRUE(ArchiveProvider::isArchivePath("foo.zip"));
    EXPECT_TRUE(ArchiveProvider::isArchivePath("foo.tar.gz"));
    EXPECT_TRUE(ArchiveProvider::isArchivePath("FOO.7Z"));
    EXPECT_TRUE(ArchiveProvider::isArchivePath("foo.rar"));
    EXPECT_FALSE(ArchiveProvider::isArchivePath("foo.txt"));
}

TEST(ArchiveProviderTest, IsReadOnly) {
    QTemporaryDir workDir;
    ASSERT_TRUE(workDir.isValid());
    const QString archivePath = QDir(workDir.path()).filePath("ro.zip");
    QString err;
    QTemporaryDir srcDir;
    writeFile(srcDir.path(), "f.txt", "x");
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {QDir(srcDir.path()).filePath("f.txt")}, "zip",
                                       &err))
        << err.toStdString();

    ArchiveProvider provider(archivePath, &err);
    ASSERT_TRUE(provider.isValid());
    EXPECT_TRUE(provider.canStream());
    QString newPath;
    EXPECT_EQ(provider.rename("/f.txt", "g.txt", &newPath), FileProvider::RenameResult::Failed);
    EXPECT_EQ(provider.openWrite("/f.txt", true), nullptr);
    EXPECT_FALSE(provider.remove("/f.txt"));
    EXPECT_FALSE(provider.mkdir("/newdir"));
}

TEST(ArchiveProviderTest, InvalidArchiveReportsError) {
    QString err;
    ArchiveProvider provider("/nonexistent/nope.zip", &err);
    EXPECT_FALSE(provider.isValid());
    EXPECT_FALSE(err.isEmpty());
}
