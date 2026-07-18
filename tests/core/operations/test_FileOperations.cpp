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
