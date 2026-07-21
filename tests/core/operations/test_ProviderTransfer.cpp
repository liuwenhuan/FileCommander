#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FileOperations.h"
#include "LocalFileProvider.h"

// Cross-provider transfer engine tests. There is no SFTP server available in
// CI, so both sides use LocalFileProvider — the streaming/resume code path is
// provider-agnostic, so exercising it locally covers the same logic a
// local<->remote transfer runs.
namespace {

QString writeFile(const QString &dir, const QString &name, const QByteArray &content) {
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
    return path;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

// A payload with position-dependent bytes so any duplication, truncation, or
// misaligned resume corrupts the content in a way the assertions catch.
QByteArray patternedPayload(int size) {
    QByteArray data;
    data.resize(size);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>(i % 251);
    return data;
}

} // namespace

TEST(ProviderTransferTest, CopiesSingleFileByteForByte) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload = patternedPayload(200000); // spans many 64 KiB chunks
    const QString source = writeFile(srcDir.path(), "file.bin", payload);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    EXPECT_TRUE(QFile::exists(source)); // copy leaves the source in place
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("file.bin")), payload);
}

TEST(ProviderTransferTest, CopiesDirectoryRecursively) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QDir(srcDir.path()).mkdir("tree");
    const QString treeRoot = QDir(srcDir.path()).filePath("tree");
    QDir(treeRoot).mkdir("nested");
    const QByteArray top = QByteArray("top-level file");
    const QByteArray inner = QByteArray("deeply nested file");
    writeFile(treeRoot, "top.txt", top);
    writeFile(QDir(treeRoot).filePath("nested"), "inner.txt", inner);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {treeRoot}, provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("tree/top.txt")), top);
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("tree/nested/inner.txt")), inner);
}

TEST(ProviderTransferTest, ResumesPartialDestination) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload = patternedPayload(300000);
    const QString source = writeFile(srcDir.path(), "resume.bin", payload);

    // Simulate an interrupted transfer: the destination already holds the first
    // N bytes of the source (a valid prefix).
    const int partial = 123456;
    writeFile(dstDir.path(), "resume.bin", payload.left(partial));

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                        /*removeSource=*/false, nullptr, &err))
        << err.toStdString();

    // The final file must equal the whole source — neither duplicated (which
    // would grow it past payload.size()) nor corrupted at the resume seam.
    const QByteArray result = readFile(QDir(dstDir.path()).filePath("resume.bin"));
    EXPECT_EQ(result.size(), payload.size());
    EXPECT_EQ(result, payload);
}

TEST(ProviderTransferTest, MoveRemovesSource) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QByteArray payload = QByteArray("payload to relocate");
    const QString source = writeFile(srcDir.path(), "move.txt", payload);

    auto *provider = LocalFileProvider::instance();
    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyAcrossProviders(provider, {source}, provider, dstDir.path(),
                                        /*removeSource=*/true, nullptr, &err))
        << err.toStdString();

    EXPECT_FALSE(QFile::exists(source)); // move removes the source
    EXPECT_EQ(readFile(QDir(dstDir.path()).filePath("move.txt")), payload);
}
