#include <gtest/gtest.h>

#include <QStandardPaths>
#include <QTemporaryDir>

#include "SessionManager.h"

namespace {
// Redirects QStandardPaths::GenericConfigLocation to an isolated temp dir
// for the lifetime of the test, so this never touches the real user's
// ~/.config/FileCommander/session.ini.
//
// Deliberately does NOT use QStandardPaths::setTestModeEnabled(true): that
// makes GenericConfigLocation resolve to a fixed ~/.qttest/config path
// instead of honoring XDG_CONFIG_HOME, which defeats the per-test
// isolation this class exists to provide (and leaks state across runs).
class IsolatedConfigDir {
public:
    IsolatedConfigDir() { qputenv("XDG_CONFIG_HOME", dir.path().toUtf8()); }

    QTemporaryDir dir;
};
} // namespace

TEST(SessionManagerTest, LoadReturnsFalseWhenNothingSaved) {
    IsolatedConfigDir isolated;
    SessionPanelData left, right;
    EXPECT_FALSE(SessionManager::load(left, right));
}

TEST(SessionManagerTest, SaveThenLoadRoundTrips) {
    IsolatedConfigDir isolated;

    SessionTabData docsTab;
    docsTab.path = "/home/user/docs";
    docsTab.selectedFiles = QStringList("/home/user/docs/a.txt");
    SessionTabData picsTab;
    picsTab.path = "/home/user/pics";

    SessionPanelData left;
    left.activeTab = 1;
    left.tabs.append(docsTab);
    left.tabs.append(picsTab);

    SessionTabData tmpTab;
    tmpTab.path = "/tmp";
    tmpTab.selectedFiles = QStringList{"/tmp/x.txt", "/tmp/y.txt"};

    SessionPanelData right;
    right.activeTab = 0;
    right.tabs.append(tmpTab);

    SessionManager::save(left, right);

    SessionPanelData loadedLeft, loadedRight;
    ASSERT_TRUE(SessionManager::load(loadedLeft, loadedRight));

    EXPECT_EQ(loadedLeft.activeTab, 1);
    ASSERT_EQ(loadedLeft.tabs.size(), 2);
    EXPECT_EQ(loadedLeft.tabs.at(0).path, QString("/home/user/docs"));
    EXPECT_EQ(loadedLeft.tabs.at(0).selectedFiles,
              QStringList{"/home/user/docs/a.txt"});
    EXPECT_EQ(loadedLeft.tabs.at(1).path, QString("/home/user/pics"));

    EXPECT_EQ(loadedRight.activeTab, 0);
    ASSERT_EQ(loadedRight.tabs.size(), 1);
    EXPECT_EQ(loadedRight.tabs.at(0).selectedFiles.size(), 2);
}

// A later save must not leave the earlier one's keys behind. QSettings' array
// writer overwrites entry-by-entry rather than replacing the array, so a shrunk
// tab list used to keep the dropped tabs' keys -- and, worse, a local tab
// landing on an index that previously held a network tab inherited that tab's
// conn/* keys and came back looking remote (so dropNetworkTabs() would discard
// a perfectly good local tab).
TEST(SessionManagerTest, ResavingReplacesTheEarlierSessionInsteadOfMergingWithIt) {
    IsolatedConfigDir isolated;

    SessionTabData home;
    home.path = "/home/user";
    SessionTabData remote;
    remote.path = "/";
    remote.conn.host = "nas.local";
    remote.conn.user = "someone";
    remote.conn.id = "bookmark-id";
    SessionTabData tmp;
    tmp.path = "/tmp";

    SessionPanelData left;
    left.tabs = {home, remote, tmp}; // network tab sits at index 1
    left.activeTab = 0;
    SessionPanelData right;
    right.tabs = {home};
    SessionManager::save(left, right);

    // Now save a shorter, all-local session -- /tmp moves onto the index the
    // network tab used to occupy.
    SessionPanelData shrunk;
    shrunk.tabs = {home, tmp};
    shrunk.activeTab = 1;
    SessionManager::save(shrunk, right);

    SessionPanelData loadedLeft, loadedRight;
    ASSERT_TRUE(SessionManager::load(loadedLeft, loadedRight));
    ASSERT_EQ(loadedLeft.tabs.size(), 2); // the third tab must be gone
    EXPECT_EQ(loadedLeft.tabs.at(1).path, QString("/tmp"));
    EXPECT_TRUE(loadedLeft.tabs.at(1).conn.host.isEmpty()); // and still local
    EXPECT_TRUE(loadedLeft.tabs.at(1).conn.id.isEmpty());

    // The reloaded local tabs survive the network filter.
    SessionManager::dropNetworkTabs(loadedLeft);
    EXPECT_EQ(loadedLeft.tabs.size(), 2);
}

namespace {
SessionTabData localTab(const QString &path) {
    SessionTabData t;
    t.path = path;
    return t;
}

SessionTabData networkTab(const QString &host) {
    SessionTabData t;
    t.path = "/";
    t.conn.host = host; // non-empty host is what marks a tab as remote
    return t;
}

QStringList paths(const SessionPanelData &panel) {
    QStringList out;
    for (const SessionTabData &t : panel.tabs)
        out << t.path;
    return out;
}
} // namespace

TEST(SessionManagerDropNetworkTabsTest, LeavesAnAllLocalPanelAlone) {
    SessionPanelData panel;
    panel.tabs = {localTab("/a"), localTab("/b"), localTab("/c")};
    panel.activeTab = 2;

    SessionManager::dropNetworkTabs(panel);

    EXPECT_EQ(paths(panel), (QStringList{"/a", "/b", "/c"}));
    EXPECT_EQ(panel.activeTab, 2);
}

TEST(SessionManagerDropNetworkTabsTest, KeepsLocalTabsAndShiftsActiveIndexDown) {
    // The active tab (/c, index 3) survives, but a network tab before it goes,
    // so its index has to move down with it.
    SessionPanelData panel;
    panel.tabs = {localTab("/a"), networkTab("nas.local"), localTab("/b"), localTab("/c")};
    panel.activeTab = 3;

    SessionManager::dropNetworkTabs(panel);

    EXPECT_EQ(paths(panel), (QStringList{"/a", "/b", "/c"}));
    EXPECT_EQ(panel.activeTab, 2);
}

TEST(SessionManagerDropNetworkTabsTest, ActiveNetworkTabFallsThroughToTheNextSurvivor) {
    SessionPanelData panel;
    panel.tabs = {localTab("/a"), networkTab("nas.local"), localTab("/b")};
    panel.activeTab = 1; // the tab being dropped

    SessionManager::dropNetworkTabs(panel);

    EXPECT_EQ(paths(panel), (QStringList{"/a", "/b"}));
    EXPECT_EQ(panel.activeTab, 1); // /b, which took the vacated slot
}

TEST(SessionManagerDropNetworkTabsTest, ActiveTrailingNetworkTabClampsToTheLastSurvivor) {
    // Nothing follows the dropped active tab, so the index would run past the
    // end if it weren't clamped.
    SessionPanelData panel;
    panel.tabs = {localTab("/a"), localTab("/b"), networkTab("nas.local")};
    panel.activeTab = 2;

    SessionManager::dropNetworkTabs(panel);

    EXPECT_EQ(paths(panel), (QStringList{"/a", "/b"}));
    EXPECT_EQ(panel.activeTab, 1);
}

TEST(SessionManagerDropNetworkTabsTest, AnAllNetworkPanelEmptiesOutWithASafeActiveIndex) {
    // The caller falls back to a local path for an empty panel; all this has to
    // guarantee is that it doesn't hand over an out-of-range index.
    SessionPanelData panel;
    panel.tabs = {networkTab("nas.local"), networkTab("ftp.example.com")};
    panel.activeTab = 1;

    SessionManager::dropNetworkTabs(panel);

    EXPECT_TRUE(panel.tabs.isEmpty());
    EXPECT_EQ(panel.activeTab, 0);
}

TEST(SessionManagerDropNetworkTabsTest, PanelsAreFilteredIndependently) {
    IsolatedConfigDir isolated;

    SessionPanelData left;
    left.tabs = {networkTab("nas.local"), localTab("/home/user")};
    left.activeTab = 0;

    SessionPanelData right;
    right.tabs = {localTab("/tmp"), networkTab("ftp.example.com")};
    right.activeTab = 1;

    SessionManager::save(left, right);

    SessionPanelData loadedLeft, loadedRight;
    ASSERT_TRUE(SessionManager::load(loadedLeft, loadedRight));
    // load() itself stays faithful to the file -- the network tabs are still there.
    EXPECT_EQ(loadedLeft.tabs.size(), 2);
    EXPECT_EQ(loadedRight.tabs.size(), 2);

    SessionManager::dropNetworkTabs(loadedLeft);
    SessionManager::dropNetworkTabs(loadedRight);

    EXPECT_EQ(paths(loadedLeft), (QStringList{"/home/user"}));
    EXPECT_EQ(loadedLeft.activeTab, 0);
    EXPECT_EQ(paths(loadedRight), (QStringList{"/tmp"}));
    EXPECT_EQ(loadedRight.activeTab, 0);
}
