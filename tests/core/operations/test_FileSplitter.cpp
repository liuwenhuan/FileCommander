#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "FileSplitter.h"

namespace {
QByteArray readAll(const QString &path) {
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}
} // namespace

TEST(FileSplitterTest, SplitProducesCorrectlySizedParts) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = QDir(dir.path()).filePath("data.bin");
    QFile f(source);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(5000, 'A'));
    f.close();

    QString err;
    const QStringList parts = FileSplitter::split(source, 2000, dir.path(), &err);
    ASSERT_EQ(parts.size(), 3) << err.toStdString();
    EXPECT_EQ(QFileInfo(parts.at(0)).fileName(), "data.bin.001");
    EXPECT_EQ(QFileInfo(parts.at(0)).size(), 2000);
    EXPECT_EQ(QFileInfo(parts.at(1)).size(), 2000);
    EXPECT_EQ(QFileInfo(parts.at(2)).size(), 1000);
}

TEST(FileSplitterTest, SplitThenMergeRoundTrips) {
    QTemporaryDir dir, out;
    ASSERT_TRUE(dir.isValid() && out.isValid());
    const QString source = QDir(dir.path()).filePath("movie.mp4");
    QByteArray content;
    for (int i = 0; i < 4096; ++i)
        content.append(static_cast<char>(i % 251));
    QFile f(source);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();

    const QStringList parts = FileSplitter::split(source, 1000, dir.path());
    ASSERT_FALSE(parts.isEmpty());

    const QString merged = QDir(out.path()).filePath("movie.mp4");
    ASSERT_TRUE(FileSplitter::merge(parts.first(), merged));
    EXPECT_EQ(readAll(merged), content);
}

TEST(FileSplitterTest, BaseNameForPartStripsNumericSuffix) {
    EXPECT_EQ(FileSplitter::baseNameForPart("/a/movie.mp4.001"), "movie.mp4");
    EXPECT_EQ(FileSplitter::baseNameForPart("/a/archive.7z.014"), "archive.7z");
    EXPECT_TRUE(FileSplitter::baseNameForPart("/a/notapart.txt").isEmpty());
}
