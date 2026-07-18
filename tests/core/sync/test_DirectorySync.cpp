#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "DirectorySync.h"

namespace {
void writeFile(const QString &dir, const QString &relPath, const QByteArray &content) {
    const QString path = QDir(dir).filePath(relPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
}

const SyncEntry *findEntry(const QVector<SyncEntry> &entries, const QString &relPath) {
    for (const auto &e : entries) {
        if (e.relativePath == relPath)
            return &e;
    }
    return nullptr;
}
} // namespace

TEST(DirectorySyncTest, DetectsLeftOnlyAndRightOnly) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "only_left.txt", "a");
    writeFile(right.path(), "only_right.txt", "b");

    const auto entries = DirectorySync::compare(left.path(), right.path(), /*recursive=*/false);
    ASSERT_EQ(entries.size(), 2);

    const auto *l = findEntry(entries, "only_left.txt");
    ASSERT_TRUE(l);
    EXPECT_EQ(l->status, SyncEntry::Status::LeftOnly);

    const auto *r = findEntry(entries, "only_right.txt");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, SyncEntry::Status::RightOnly);
}

TEST(DirectorySyncTest, DetectsIdenticalFilesAsSame) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "same.txt", "identical content");
    writeFile(right.path(), "same.txt", "identical content");

    const auto entries = DirectorySync::compare(left.path(), right.path(), false);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().status, SyncEntry::Status::Same);
}

TEST(DirectorySyncTest, DetectsDifferentSizeAsDifferent) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "changed.txt", "short");
    writeFile(right.path(), "changed.txt", "a much longer piece of content");

    const auto entries = DirectorySync::compare(left.path(), right.path(), false);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().status, SyncEntry::Status::Different);
    EXPECT_NE(entries.first().leftSize, entries.first().rightSize);
}

TEST(DirectorySyncTest, RecursiveFindsNestedFiles) {
    QTemporaryDir left, right;
    ASSERT_TRUE(left.isValid() && right.isValid());
    writeFile(left.path(), "sub/dir/nested.txt", "nested");

    auto shallow = DirectorySync::compare(left.path(), right.path(), /*recursive=*/false);
    EXPECT_TRUE(shallow.isEmpty());

    auto deep = DirectorySync::compare(left.path(), right.path(), /*recursive=*/true);
    ASSERT_EQ(deep.size(), 1);
    EXPECT_EQ(deep.first().relativePath, QString("sub/dir/nested.txt"));
    EXPECT_EQ(deep.first().status, SyncEntry::Status::LeftOnly);
}
