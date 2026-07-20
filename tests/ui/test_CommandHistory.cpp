#include <gtest/gtest.h>

#include "CommandHistory.h"

TEST(CommandHistoryTest, UpWalksBackwardThenClampsAtOldest) {
    CommandHistory h;
    h.add("one");
    h.add("two");
    h.add("three");

    EXPECT_EQ(h.older(""), "three");
    EXPECT_EQ(h.older("three"), "two");
    EXPECT_EQ(h.older("two"), "one");
    EXPECT_EQ(h.older("one"), "one"); // clamped at the oldest entry
}

TEST(CommandHistoryTest, DownWalksForwardToFreshLine) {
    CommandHistory h;
    h.add("a");
    h.add("b");

    EXPECT_EQ(h.older(""), "b");
    EXPECT_EQ(h.older("b"), "a");
    EXPECT_EQ(h.newer(), "b");
    EXPECT_EQ(h.newer(), QString()); // back to the empty editing line
    EXPECT_EQ(h.newer(), QString()); // stays put
}

TEST(CommandHistoryTest, AddCollapsesImmediateRepeats) {
    CommandHistory h;
    h.add("ls");
    h.add("ls");
    h.add("pwd");
    EXPECT_EQ(h.items(), (QStringList{"ls", "pwd"}));
}

TEST(CommandHistoryTest, EmptyCommandIsNotRecorded) {
    CommandHistory h;
    h.add("");
    EXPECT_TRUE(h.items().isEmpty());
    EXPECT_EQ(h.older("draft"), "draft"); // nothing to recall
}

TEST(CommandHistoryTest, AddResetsCursorToNewLine) {
    CommandHistory h;
    h.add("first");
    EXPECT_EQ(h.older(""), "first");
    // Submitting a new command should return the cursor to the fresh line,
    // so the next Up shows the newest entry, not where we left off.
    h.add("second");
    EXPECT_EQ(h.older(""), "second");
}
