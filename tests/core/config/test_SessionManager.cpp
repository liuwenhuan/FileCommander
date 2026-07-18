#include <gtest/gtest.h>

#include <QStandardPaths>
#include <QTemporaryDir>

#include "SessionManager.h"

namespace {
// Redirects QStandardPaths::GenericConfigLocation to an isolated temp dir
// for the lifetime of the test, so this never touches the real user's
// ~/.config/totalcommander/session.ini.
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
