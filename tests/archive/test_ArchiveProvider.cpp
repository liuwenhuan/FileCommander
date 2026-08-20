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

TEST(ArchiveProviderTest, LayoutTest_StripsSingleTopLevelFolder) {
    const QStringList entries = {"root/", "root/a.txt", "root/sub/b.txt"};
    const auto r = ArchiveLayout::analyze(entries, "pkg.zip");
    EXPECT_TRUE(r.stripSingleRoot);
    EXPECT_EQ(r.strippedPrefix, QString("root"));
    EXPECT_FALSE(r.wrapInArchiveNamedFolder);
}

TEST(ArchiveProviderTest, LayoutTest_WrapsMultipleTopLevelItems) {
    const QStringList entries = {"a.txt", "sub/b.txt"};
    const auto r = ArchiveLayout::analyze(entries, "pkg.zip");
    EXPECT_FALSE(r.stripSingleRoot);
    EXPECT_TRUE(r.wrapInArchiveNamedFolder);
}

TEST(ArchiveProviderTest, LayoutTest_SingleFileAtRootIsNeitherStrippedNorWrapped) {
    const QStringList entries = {"only.txt"};
    const auto r = ArchiveLayout::analyze(entries, "pkg.zip");
    EXPECT_FALSE(r.stripSingleRoot);
    EXPECT_FALSE(r.wrapInArchiveNamedFolder);
}

TEST(ArchiveProviderTest, LayoutTest_DetectsNestedArchiveAfterStrip) {
    const QStringList entries = {"root/", "root/inner.tar.gz"};
    const auto r = ArchiveLayout::analyze(entries, "outer.zip");
    EXPECT_TRUE(r.stripSingleRoot);
    EXPECT_TRUE(r.resultIsSingleArchive);
    EXPECT_EQ(r.innerArchiveName, QString("inner.tar.gz"));
}

TEST(ArchiveProviderTest, LayoutTest_DetectsSingleArchiveFileAtRoot) {
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

// --- Archives that live on a server ----------------------------------------
// libarchive opens a file by name, so one of those is browsed through a copy
// downloaded to /tmp. Two things then stop being derivable from the file's own
// location: where ".." leads, and who deletes the copy.

TEST(ArchiveProviderTest, ExitPathOverridesTheArchiveFilesOwnDirectory) {
    QTemporaryDir srcDir, workDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid());
    writeFile(srcDir.path(), "pkg/a.txt", "a");
    const QString archivePath = QDir(workDir.path()).filePath("bundle.zip");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {QDir(srcDir.path()).filePath("pkg")}, "zip",
                                       &err))
        << err.toStdString();

    ArchiveProvider provider(archivePath, &err);
    ASSERT_TRUE(provider.isValid()) << err.toStdString();
    // Default: the ".." out of the root is the directory the file sits in.
    EXPECT_EQ(provider.parentPath("/"), workDir.path());

    // A remote archive's copy is in /tmp, but the user came from a share.
    provider.setExitPath(QStringLiteral("/share/docs"));
    EXPECT_EQ(provider.parentPath("/"), QString("/share/docs"));
    // Only the root's parent changes: paths inside the archive still resolve
    // within it, so the override cannot leak into normal navigation.
    EXPECT_EQ(provider.parentPath("/a.txt"), QString("/"));
}

TEST(ArchiveProviderTest, OwnedArchiveFileIsDeletedWithTheProvider) {
    QTemporaryDir srcDir, workDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid());
    writeFile(srcDir.path(), "pkg/a.txt", "a");
    // The downloaded copy gets a directory to itself, exactly as the fetch does,
    // so that both it and the directory should be gone afterwards.
    const QString copyDir = QDir(workDir.path()).filePath("42");
    ASSERT_TRUE(QDir().mkpath(copyDir));
    const QString archivePath = QDir(copyDir).filePath("bundle.zip");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {QDir(srcDir.path()).filePath("pkg")}, "zip",
                                       &err))
        << err.toStdString();

    {
        ArchiveProvider provider(archivePath, &err);
        ASSERT_TRUE(provider.isValid()) << err.toStdString();
        provider.setOwnsArchiveFile(true);
        // Extract something too: the entry temp dir must not be what keeps the
        // copy's directory alive.
        EXPECT_EQ(readEntry(provider, "/a.txt"), QByteArray("a"));
        EXPECT_TRUE(QFile::exists(archivePath));
    }
    EXPECT_FALSE(QFile::exists(archivePath));
    EXPECT_FALSE(QDir(copyDir).exists());
}

TEST(ArchiveProviderTest, LocalArchiveFileSurvivesItsProvider) {
    QTemporaryDir srcDir, workDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid());
    writeFile(srcDir.path(), "pkg/a.txt", "a");
    const QString archivePath = QDir(workDir.path()).filePath("bundle.zip");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath, {QDir(srcDir.path()).filePath("pkg")}, "zip",
                                       &err))
        << err.toStdString();

    {
        ArchiveProvider provider(archivePath, &err);
        ASSERT_TRUE(provider.isValid()) << err.toStdString();
        EXPECT_EQ(readEntry(provider, "/a.txt"), QByteArray("a"));
    }
    // Nobody said we owned it: browsing the user's own archive must not eat it.
    EXPECT_TRUE(QFile::exists(archivePath));
    EXPECT_TRUE(QDir(workDir.path()).exists());
}

TEST(ArchiveProviderTest, EncryptedArchiveIsNotBrowsableWithoutItsPassword) {
    QTemporaryDir srcDir, workDir;
    ASSERT_TRUE(srcDir.isValid() && workDir.isValid());
    writeFile(srcDir.path(), "secret.txt", "top secret");
    const QString archivePath = QDir(workDir.path()).filePath("locked.zip");
    QString err;
    ASSERT_TRUE(ArchiveHandler::create(archivePath,
                                       {QDir(srcDir.path()).filePath("secret.txt")}, "zip",
                                       QStringLiteral("hunter2"), /*encryptHeaders=*/false,
                                       /*compressionLevel=*/5, &err))
        << err.toStdString();

    // No password: refuse the browse instead of listing names whose contents
    // would every one of them read back as ciphertext.
    ArchiveProvider blind(archivePath);
    EXPECT_FALSE(blind.isValid());
    EXPECT_EQ(blind.status(), ArchiveHandler::Status::NeedPassword);

    ArchiveProvider wrong(archivePath, nullptr, QStringLiteral("not-it"));
    EXPECT_FALSE(wrong.isValid());
    EXPECT_EQ(wrong.status(), ArchiveHandler::Status::WrongPassword);

    ArchiveProvider ok(archivePath, &err, QStringLiteral("hunter2"));
    ASSERT_TRUE(ok.isValid()) << err.toStdString();
    EXPECT_EQ(ok.status(), ArchiveHandler::Status::Ok);
    EXPECT_EQ(readEntry(ok, "/secret.txt"), QByteArray("top secret"));
}
