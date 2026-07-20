#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QtTest/QSignalSpy>
#include <QTemporaryDir>

#include "FileSystemModel.h"

namespace {

void touch(const QString &dir, const QString &name) {
    QFile f(QDir(dir).filePath(name));
    f.open(QIODevice::WriteOnly);
    f.write("x");
    f.close();
}

void writeBytes(const QString &path, int n) {
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray(n, 'x'));
    f.close();
}

// Drives the model's async scan to completion.
bool loadDir(FileSystemModel &model, const QString &path) {
    QSignalSpy spy(&model, &FileSystemModel::loadFinished);
    model.setRootPath(path);
    return spy.wait(2000) || spy.count() > 0;
}

// Visible real entries, excluding the synthesized ".." parent row (a temp
// dir always has a parent, so rowCount includes it).
int visibleFiles(const FileSystemModel &model) {
    int rows = model.rowCount();
    if (rows > 0 && model.isParentEntry(0))
        --rows;
    return rows;
}

} // namespace

TEST(FileSystemModelFilterTest, MatchesFilterIsCaseInsensitiveSubstring) {
    EXPECT_TRUE(FileSystemModel::matchesFilter("Report.txt", "report"));
    EXPECT_TRUE(FileSystemModel::matchesFilter("Report.txt", "PORT"));
    EXPECT_FALSE(FileSystemModel::matchesFilter("Report.txt", "xyz"));
}

TEST(FileSystemModelFilterTest, MatchesFilterEmptyMatchesEverything) {
    EXPECT_TRUE(FileSystemModel::matchesFilter("anything", ""));
}

TEST(FileSystemModelFilterTest, MatchesFilterSupportsWildcards) {
    EXPECT_TRUE(FileSystemModel::matchesFilter("photo.png", "*.png"));
    EXPECT_FALSE(FileSystemModel::matchesFilter("photo.png", "*.jpg"));
    EXPECT_TRUE(FileSystemModel::matchesFilter("a1b.txt", "a?b*"));
}

TEST(FileSystemModelFilterTest, SetNameFilterRestrictsVisibleRows) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "alpha.txt");
    touch(dir.path(), "beta.txt");
    touch(dir.path(), "alphabet.md");

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));
    EXPECT_EQ(visibleFiles(model), 3);

    model.setNameFilter("alpha");
    EXPECT_EQ(visibleFiles(model), 2); // alpha.txt + alphabet.md

    model.setNameFilter("*.txt");
    EXPECT_EQ(visibleFiles(model), 2); // alpha.txt + beta.txt

    model.setNameFilter("");
    EXPECT_EQ(visibleFiles(model), 3);
}

TEST(FileSystemModelFilterTest, PatternPicksExpectedEntriesAcrossRows) {
    // Mirrors FilePanel::selectByPattern: iterate the model's rows and match
    // each name against a wildcard mask.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "notes.txt");
    touch(dir.path(), "todo.txt");
    touch(dir.path(), "image.png");

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));

    QStringList matched;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.isParentEntry(r))
            continue;
        const QString name = model.fileInfoAt(r).name();
        if (FileSystemModel::matchesFilter(name, "*.txt"))
            matched << name;
    }
    matched.sort();
    EXPECT_EQ(matched, (QStringList{"notes.txt", "todo.txt"}));
}

TEST(FileSystemModelSizeTest, DirectorySizeSumsRecursively) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeBytes(QDir(dir.path()).filePath("a.bin"), 100);
    ASSERT_TRUE(QDir(dir.path()).mkdir("sub"));
    writeBytes(QDir(dir.path()).filePath("sub/b.bin"), 200);

    EXPECT_EQ(FileSystemModel::directorySize(dir.path()), 300);
}

TEST(FileSystemModelSizeTest, ComputedSizeReplacesDirPlaceholderInSizeColumn) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("folder"));
    writeBytes(QDir(dir.path()).filePath("folder/inner.bin"), 2048);

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));

    // Locate the row for "folder".
    int folderRow = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.fileInfoAt(r).name() == "folder") {
            folderRow = r;
            break;
        }
    }
    ASSERT_GE(folderRow, 0);

    const QModelIndex sizeIdx = model.index(folderRow, FileSystemModel::SizeColumn);
    EXPECT_EQ(model.data(sizeIdx).toString().toStdString(), "<DIR>");

    model.setComputedDirSize(QDir(dir.path()).filePath("folder"), 2048);
    EXPECT_EQ(model.data(sizeIdx).toString().toStdString(), "2.0 KB");
}

TEST(FileSystemModelFilterTest, ChangingDirectoryClearsFilter) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.path(), "one.txt");
    touch(dir.path(), "two.log");

    FileSystemModel model;
    ASSERT_TRUE(loadDir(model, dir.path()));
    model.setNameFilter("one");
    EXPECT_EQ(visibleFiles(model), 1);

    // Re-scanning (navigation / refresh) must reset the filter.
    ASSERT_TRUE(loadDir(model, dir.path()));
    EXPECT_TRUE(model.nameFilter().isEmpty());
    EXPECT_EQ(visibleFiles(model), 2);
}
