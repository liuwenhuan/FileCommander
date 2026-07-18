#include <gtest/gtest.h>

#include "TextDiff.h"

TEST(TextDiffTest, IdenticalLinesAreAllSame) {
    QStringList left = {"a", "b", "c"};
    QStringList right = {"a", "b", "c"};

    const auto diff = TextDiff::compare(left, right);
    ASSERT_EQ(diff.size(), 3);
    for (const auto &line : diff)
        EXPECT_EQ(line.kind, DiffLine::Kind::Same);
}

TEST(TextDiffTest, DetectsAddedLine) {
    QStringList left = {"a", "c"};
    QStringList right = {"a", "b", "c"};

    const auto diff = TextDiff::compare(left, right);
    ASSERT_EQ(diff.size(), 3);
    EXPECT_EQ(diff.at(0).kind, DiffLine::Kind::Same);
    EXPECT_EQ(diff.at(1).kind, DiffLine::Kind::Added);
    EXPECT_EQ(diff.at(1).rightText, QString("b"));
    EXPECT_EQ(diff.at(2).kind, DiffLine::Kind::Same);
}

TEST(TextDiffTest, DetectsRemovedLine) {
    QStringList left = {"a", "b", "c"};
    QStringList right = {"a", "c"};

    const auto diff = TextDiff::compare(left, right);
    ASSERT_EQ(diff.size(), 3);
    EXPECT_EQ(diff.at(1).kind, DiffLine::Kind::Removed);
    EXPECT_EQ(diff.at(1).leftText, QString("b"));
}

TEST(TextDiffTest, EmptyInputsProduceEmptyDiff) {
    EXPECT_TRUE(TextDiff::compare({}, {}).isEmpty());
}

TEST(TextDiffTest, CompletelyDifferentFilesShowAllRemovedThenAdded) {
    QStringList left = {"x", "y"};
    QStringList right = {"p", "q"};

    const auto diff = TextDiff::compare(left, right);
    ASSERT_EQ(diff.size(), 4);
    int removed = 0, added = 0;
    for (const auto &line : diff) {
        if (line.kind == DiffLine::Kind::Removed)
            ++removed;
        else if (line.kind == DiffLine::Kind::Added)
            ++added;
    }
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(added, 2);
}
