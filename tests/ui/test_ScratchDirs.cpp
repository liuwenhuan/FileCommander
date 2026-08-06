#include <gtest/gtest.h>

#include <QDir>
#include <QFile>

#include "ScratchDirs.h"

// The one real difference between the three directories is whether what is in
// them outlives the program, and that difference was previously expressed by a
// single setAutoRemove call buried in one of three copied functions. These are
// the tests it never had.

TEST(ScratchDirsTest, EachKindGetsItsOwnDirectoryAndTheSameOneEveryTime) {
    ScratchDirs dirs;
    const QString preview = dirs.preview();
    const QString openWith = dirs.openWith();
    const QString archive = dirs.archive();

    ASSERT_FALSE(preview.isEmpty());
    ASSERT_FALSE(openWith.isEmpty());
    ASSERT_FALSE(archive.isEmpty());
    EXPECT_NE(preview, openWith);
    EXPECT_NE(openWith, archive);
    EXPECT_NE(preview, archive);

    // Created once, not per call: the open-with directory holds every fetched
    // copy for the session, so a second call handing back a different one would
    // scatter them.
    EXPECT_EQ(dirs.openWith(), openWith);
    EXPECT_EQ(dirs.preview(), preview);
}

TEST(ScratchDirsTest, PreviewAndArchiveCopiesGoWhenTheProgramDoes) {
    QString preview;
    QString archive;
    {
        ScratchDirs dirs;
        preview = dirs.preview();
        archive = dirs.archive();
        ASSERT_TRUE(QDir(preview).exists());
        ASSERT_TRUE(QDir(archive).exists());
    }
    // Nothing outside the process reads either, and an archive copy can be
    // several gigabytes.
    EXPECT_FALSE(QDir(preview).exists());
    EXPECT_FALSE(QDir(archive).exists());
}

TEST(ScratchDirsTest, CopiesHandedToOtherApplicationsSurviveTheProgram) {
    QString openWith;
    {
        ScratchDirs dirs;
        openWith = dirs.openWith();
        QFile handed(QDir(openWith).filePath(QStringLiteral("document.txt")));
        ASSERT_TRUE(handed.open(QIODevice::WriteOnly));
        handed.write("still open in another application");
    }
    // An application we launched may still be reading its copy. Leaving bytes
    // in /tmp, which the system clears anyway, beats pulling a file out from
    // under a document the user is looking at.
    EXPECT_TRUE(QDir(openWith).exists())
        << "the open-with copies were removed with the program";
    EXPECT_TRUE(QFile::exists(QDir(openWith).filePath(QStringLiteral("document.txt"))));

    QDir(openWith).removeRecursively(); // this test's own litter, not the app's
}

TEST(ScratchDirsTest, DiscardingArchivesSweepsThemUpAndAllowsAFreshOne) {
    ScratchDirs dirs;
    const QString first = dirs.archive();
    ASSERT_TRUE(QDir(first).exists());

    // What closeEvent does: a browse the user never stepped out of leaves a
    // copy behind, and these are ours to remove.
    dirs.discardArchive();
    EXPECT_FALSE(QDir(first).exists());

    const QString second = dirs.archive();
    EXPECT_FALSE(second.isEmpty());
    EXPECT_NE(second, first);
    EXPECT_TRUE(QDir(second).exists());
}
