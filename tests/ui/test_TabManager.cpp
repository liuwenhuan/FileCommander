#include <gtest/gtest.h>

#include "TabManager.h"

// Regression test for the crash class that killed the old project's
// TabManager: storing TabState (which owns a QStringList) by value in a
// QList<TabState> corrupted memory on teardown. TabManager instead uses
// QVector<QSharedPointer<TabState>>, so repeated populate/destroy cycles
// must complete cleanly.
TEST(TabManagerTest, RepeatedCreatePopulateDestroyDoesNotCrash) {
    for (int cycle = 0; cycle < 200; ++cycle) {
        TabManager manager;
        for (int i = 0; i < 10; ++i) {
            int index = manager.addTab(QString("/tmp/tab%1").arg(i));
            auto state = manager.tabAt(index);
            ASSERT_TRUE(state);
            state->selectedFiles = QStringList{"a.txt", "b.txt", "c.txt"};
        }
        manager.setActiveIndex(5);
        manager.closeTab(2);
        manager.closeTab(0);
        // TabManager destructs here at end of scope on every iteration.
    }
    SUCCEED();
}

TEST(TabManagerTest, AddTabReturnsSequentialIndices) {
    TabManager manager;
    EXPECT_EQ(manager.addTab("/a"), 0);
    EXPECT_EQ(manager.addTab("/b"), 1);
    EXPECT_EQ(manager.addTab("/c"), 2);
    EXPECT_EQ(manager.count(), 3);
}

TEST(TabManagerTest, CloseTabRemovesCorrectEntry) {
    TabManager manager;
    manager.addTab("/a");
    manager.addTab("/b");
    manager.addTab("/c");
    manager.closeTab(1);
    ASSERT_EQ(manager.count(), 2);
    EXPECT_EQ(manager.tabAt(0)->path, QString("/a"));
    EXPECT_EQ(manager.tabAt(1)->path, QString("/c"));
}

TEST(TabManagerTest, ActiveIndexClampsWhenTabsCloseFromTheEnd) {
    TabManager manager;
    manager.addTab("/a");
    manager.addTab("/b");
    manager.setActiveIndex(1);
    manager.closeTab(1);
    EXPECT_EQ(manager.activeIndex(), 0);
}

TEST(TabManagerTest, CloseOthersKeepsOnlyTargetTab) {
    TabManager manager;
    manager.addTab("/a");
    manager.addTab("/b");
    manager.addTab("/c");
    manager.closeOthers(1);
    ASSERT_EQ(manager.count(), 1);
    EXPECT_EQ(manager.tabAt(0)->path, QString("/b"));
}

TEST(TabManagerTest, CloseToRightRemovesTrailingTabs) {
    TabManager manager;
    manager.addTab("/a");
    manager.addTab("/b");
    manager.addTab("/c");
    manager.closeToRight(0);
    ASSERT_EQ(manager.count(), 1);
    EXPECT_EQ(manager.tabAt(0)->path, QString("/a"));
}

TEST(TabManagerTest, ActiveTabReflectsSetActiveIndex) {
    TabManager manager;
    manager.addTab("/a");
    manager.addTab("/b");
    manager.setActiveIndex(1);
    ASSERT_TRUE(manager.activeTab());
    EXPECT_EQ(manager.activeTab()->path, QString("/b"));
}
