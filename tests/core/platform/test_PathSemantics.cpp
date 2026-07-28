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
