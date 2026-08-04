#include <gtest/gtest.h>

#include "PathSemantics.h"

TEST(PathSemantics, RecognizesPosixDriveAndUncRoots) {
    EXPECT_TRUE(PathSemantics::isRoot("/", PathFlavor::Posix));
    EXPECT_TRUE(PathSemantics::isRoot("C:\\", PathFlavor::Windows));
    EXPECT_TRUE(PathSemantics::isRoot("\\\\server\\share", PathFlavor::Windows));
    EXPECT_FALSE(PathSemantics::isRoot("C:\\data", PathFlavor::Windows));
}

TEST(PathSemantics, NormalizesWindowsSeparatorsAndCase) {
    EXPECT_TRUE(PathSemantics::equivalent("C:/Data/File.txt", "c:\\data\\file.TXT",
                                          PathFlavor::Windows));
    EXPECT_EQ(PathSemantics::parentPath("C:\\Data\\File.txt", PathFlavor::Windows),
              QString("C:\\Data"));
}

TEST(PathSemantics, RejectsWindowsReservedAndTrailingCharacters) {
    EXPECT_FALSE(PathSemantics::validateComponent("CON.txt", PathFlavor::Windows).ok);
    EXPECT_FALSE(PathSemantics::validateComponent("report. ", PathFlavor::Windows).ok);
    EXPECT_TRUE(PathSemantics::validateComponent(QString::fromUtf8("资料.txt"),
                                                 PathFlavor::Windows).ok);
}

TEST(PathSemantics, DetectsCaseOnlyRename) {
    EXPECT_TRUE(PathSemantics::requiresCaseOnlyRename("C:\\a.txt", "c:\\A.TXT",
                                                      PathFlavor::Windows));
    EXPECT_FALSE(PathSemantics::requiresCaseOnlyRename("/a", "/A", PathFlavor::Posix));
}

// Copying a directory into a place inside itself is the destination that cannot
// work: the copy writes into the tree it is still reading and recurses into its
// own output. Observed as a move of X into X/X that reached 1.9 GB before
// anything complained -- nothing did, because every individual file copy along
// the way is legal.
TEST(PathSemanticsTest, ADirectoryIsInsideItself) {
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("/home/ann/photos"),
                                              QStringLiteral("/home/ann/photos"),
                                              PathFlavor::Posix));
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("/home/ann/photos/2024"),
                                              QStringLiteral("/home/ann/photos"),
                                              PathFlavor::Posix));
    // The case that started this: the destination is a child named after the
    // source, which is what a move-into-itself produces.
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("/d/tools/tools"),
                                              QStringLiteral("/d/tools"),
                                              PathFlavor::Posix));
}

TEST(PathSemanticsTest, ASiblingIsNotInside) {
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QStringLiteral("/home/ann/music"),
                                               QStringLiteral("/home/ann/photos"),
                                               PathFlavor::Posix));
    // Upwards is not inside either: copying a directory into its own parent is
    // an ordinary, legal operation.
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QStringLiteral("/home/ann"),
                                               QStringLiteral("/home/ann/photos"),
                                               PathFlavor::Posix));
}

// A string prefix is not an ancestor. Rejecting "/home/annex" as being inside
// "/home/ann" would refuse a perfectly good copy.
TEST(PathSemanticsTest, ANameThatMerelyStartsTheSameIsNotInside) {
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QStringLiteral("/home/annex"),
                                               QStringLiteral("/home/ann"),
                                               PathFlavor::Posix));
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QStringLiteral("C:/data/backup2"),
                                               QStringLiteral("C:/data/backup"),
                                               PathFlavor::Windows));
}

// Windows compares case-insensitively and accepts either separator, so the same
// place spelled differently still has to be recognised.
TEST(PathSemanticsTest, WindowsSpellingsOfTheSamePlaceAreInside) {
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("C:\\Users\\Ann\\Docs\\Sub"),
                                              QStringLiteral("C:/users/ann/docs"),
                                              PathFlavor::Windows));
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("C:/Data/Tools/Tools"),
                                              QStringLiteral("C:/Data/Tools/"),
                                              PathFlavor::Windows));
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QStringLiteral("D:/Data/Tools/Sub"),
                                              QStringLiteral("C:/Data/Tools"),
                                              PathFlavor::Windows));
}

// A root is an ancestor of everything on it, and appending a separator to one
// that already ends in a separator must not break the comparison.
TEST(PathSemanticsTest, ARootContainsEverythingBelowIt) {
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("/home/ann"),
                                              QStringLiteral("/"), PathFlavor::Posix));
    EXPECT_TRUE(PathSemantics::isInsideOrSame(QStringLiteral("C:/Users"),
                                              QStringLiteral("C:\\"), PathFlavor::Windows));
}

TEST(PathSemanticsTest, AnEmptyPathIsNotInsideAnything) {
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QString(), QStringLiteral("/home"),
                                               PathFlavor::Posix));
    EXPECT_FALSE(PathSemantics::isInsideOrSame(QStringLiteral("/home"), QString(),
                                               PathFlavor::Posix));
}
