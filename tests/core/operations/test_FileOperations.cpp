#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FileOperations.h"

namespace {

QString writeFile(const QString &dir, const QString &name, const QByteArray &content = "data") {
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
    return path;
}

} // namespace

TEST(FileOperationsTest, CopyPathsCopiesFileWithoutRemovingSource) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "a.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    EXPECT_TRUE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("a.txt")));
}

TEST(FileOperationsTest, MovePathsRemovesSource) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "b.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.movePaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    EXPECT_FALSE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("b.txt")));
}

TEST(FileOperationsTest, ConflictResolverSkipLeavesDestinationUntouched) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "c.txt", "new");
    writeFile(dstDir.path(), "c.txt", "original");

    FileOperations ops;
    ConflictResolver resolver = [](const QString &, const QString &) {
        return ErrorAction::Skip;
    };
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), resolver, &err));

    QFile dest(QDir(dstDir.path()).filePath("c.txt"));
    dest.open(QIODevice::ReadOnly);
    EXPECT_EQ(dest.readAll(), QByteArray("original"));
}

TEST(FileOperationsTest, ConflictResolverOverwriteReplacesDestination) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "d.txt", "new");
    writeFile(dstDir.path(), "d.txt", "original");

    FileOperations ops;
    ConflictResolver resolver = [](const QString &, const QString &) {
        return ErrorAction::Overwrite;
    };
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), resolver, &err));

    QFile dest(QDir(dstDir.path()).filePath("d.txt"));
    dest.open(QIODevice::ReadOnly);
    EXPECT_EQ(dest.readAll(), QByteArray("new"));
}

TEST(FileOperationsTest, CopyPathsOntoSelfKeepsOriginalAndMakesRenamedDuplicate) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "self.txt", "payload");

    FileOperations ops;
    QString err;
    // Copying into the directory the file already lives in must not destroy it.
    ASSERT_TRUE(ops.copyPaths({source}, dir.path(), nullptr, &err)) << err.toStdString();

    QFile original(source);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("payload"));

    QFile duplicate(QDir(dir.path()).filePath("self (1).txt"));
    ASSERT_TRUE(duplicate.open(QIODevice::ReadOnly));
    EXPECT_EQ(duplicate.readAll(), QByteArray("payload"));
}

TEST(FileOperationsTest, MovePathsOntoSelfIsNoOp) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "stay.txt", "payload");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.movePaths({source}, dir.path(), nullptr, &err)) << err.toStdString();

    QFile original(source);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("payload"));
    // No spurious renamed duplicate left behind.
    EXPECT_FALSE(QFile::exists(QDir(dir.path()).filePath("stay (1).txt")));
}

TEST(FileOperationsTest, RequestCancelStopsRemainingEntries) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString a = writeFile(srcDir.path(), "a.txt");
    const QString b = writeFile(srcDir.path(), "b.txt");
    // Pre-create a conflict so the resolver runs for the first entry, where
    // we trigger cancellation mid-batch.
    writeFile(dstDir.path(), "a.txt", "existing");

    FileOperations ops;
    ConflictResolver resolver = [&ops](const QString &, const QString &) {
        ops.requestCancel();
        return ErrorAction::Skip;
    };
    QString err;
    EXPECT_FALSE(ops.copyPaths({a, b}, dstDir.path(), resolver, &err));
    EXPECT_TRUE(ops.wasCancelled());
    // The second entry must never have been processed.
    EXPECT_FALSE(QFile::exists(QDir(dstDir.path()).filePath("b.txt")));
}

TEST(FileOperationsTest, DeletePathsPermanentlyRemovesFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir.path(), "e.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.deletePaths({path}, /*toTrash=*/false, &err));
    EXPECT_FALSE(QFile::exists(path));
}

TEST(FileOperationsTest, MakeDirectoryCreatesNewFolder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.makeDirectory(dir.path(), "newdir", &err)) << err.toStdString();
    EXPECT_TRUE(QDir(dir.path()).exists("newdir"));
}

TEST(FileOperationsTest, MakeDirectoryFailsIfAlreadyExists) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("existing"));

    FileOperations ops;
    QString err;
    EXPECT_FALSE(ops.makeDirectory(dir.path(), "existing", &err));
}

TEST(FileOperationsTest, RenamePathRenamesFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir.path(), "old.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.renamePath(path, "new.txt", &err)) << err.toStdString();
    EXPECT_FALSE(QFile::exists(path));
    EXPECT_TRUE(QDir(dir.path()).exists("new.txt"));
}

TEST(FileOperationsTest, CopyPathsCopiesDirectoryRecursively) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QDir(srcDir.path()).mkdir("nested");
    writeFile(QDir(srcDir.path()).filePath("nested"), "inner.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({QDir(srcDir.path()).filePath("nested")}, dstDir.path(), nullptr,
                               &err))
        << err.toStdString();

    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("nested/inner.txt")));
}

TEST(FileOperationsTest, CreateSymlinksCreatesWorkingLink) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString target = writeFile(srcDir.path(), "target.txt", "link content");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.createSymlinks({target}, dstDir.path(), &err)) << err.toStdString();

    const QString linkPath = QDir(dstDir.path()).filePath("target.txt");
    QFileInfo linkInfo(linkPath);
    EXPECT_TRUE(linkInfo.isSymLink());
    QFile linkFile(linkPath);
    ASSERT_TRUE(linkFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(linkFile.readAll(), QByteArray("link content"));
}

TEST(FileOperationsTest, CreateSymlinksRenamesOnNameConflict) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString target = writeFile(srcDir.path(), "dup.txt", "original");
    writeFile(dstDir.path(), "dup.txt", "unrelated existing file");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.createSymlinks({target}, dstDir.path(), &err)) << err.toStdString();

    // The pre-existing dup.txt must be untouched; the link gets a renamed path instead.
    QFile existing(QDir(dstDir.path()).filePath("dup.txt"));
    ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
    EXPECT_EQ(existing.readAll(), QByteArray("unrelated existing file"));

    QFileInfo linkInfo(QDir(dstDir.path()).filePath("dup (1).txt"));
    EXPECT_TRUE(linkInfo.isSymLink());
}
