#include <gtest/gtest.h>

#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "Settings.h"

namespace {
// See test_SessionManager.cpp for why this avoids setTestModeEnabled().
class IsolatedConfigDir {
public:
    IsolatedConfigDir() { qputenv("XDG_CONFIG_HOME", dir.path().toUtf8()); }
    QTemporaryDir dir;
};
} // namespace

TEST(SettingsTest, ShortcutReturnsDefaultWhenUnset) {
    IsolatedConfigDir isolated;
    Settings settings;
    EXPECT_EQ(settings.shortcut("copy", QKeySequence(Qt::Key_F5)),
              QKeySequence(Qt::Key_F5));
}

TEST(SettingsTest, SetShortcutOverridesDefault) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.setShortcut("copy", QKeySequence(Qt::CTRL | Qt::Key_C));
    EXPECT_EQ(settings.shortcut("copy", QKeySequence(Qt::Key_F5)),
              QKeySequence(Qt::CTRL | Qt::Key_C));
}

TEST(SettingsTest, ClearShortcutOverridesRestoresDefaults) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.setShortcut("copy", QKeySequence(Qt::CTRL | Qt::Key_C));
    settings.clearShortcutOverrides();
    EXPECT_EQ(settings.shortcut("copy", QKeySequence(Qt::Key_F5)),
              QKeySequence(Qt::Key_F5));
}

TEST(SettingsTest, ListFontSizeDefaultsToTen) {
    IsolatedConfigDir isolated;
    Settings settings;
    EXPECT_EQ(settings.listFontSize(), 10);
}

TEST(SettingsTest, ListFontSizeRoundTrips) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.setListFontSize(14);
    EXPECT_EQ(settings.listFontSize(), 14);
}

TEST(SettingsTest, ListFontSizeClampsToRange) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.setListFontSize(2);
    EXPECT_EQ(settings.listFontSize(), 7);
    settings.setListFontSize(99);
    EXPECT_EQ(settings.listFontSize(), 24);
}

TEST(SettingsTest, WindowGeometryRoundTrips) {
    IsolatedConfigDir isolated;
    Settings settings;
    const QByteArray blob = "fake-geometry-bytes";
    settings.setWindowGeometry(blob);
    EXPECT_EQ(settings.windowGeometry(), blob);
}

TEST(SettingsTest, FavoriteDirectoriesDefaultToHomeOnFirstRun) {
    IsolatedConfigDir isolated;
    Settings settings;
    EXPECT_EQ(settings.favoriteDirectories(), QStringList({QDir::homePath()}));
}

TEST(SettingsTest, AddFavoriteDirectoryPersists) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.removeFavoriteDirectory(QDir::homePath()); // drop the first-run seed
    settings.addFavoriteDirectory("/home/user/projects");
    settings.addFavoriteDirectory("/tmp");
    EXPECT_EQ(settings.favoriteDirectories(),
              QStringList({"/home/user/projects", "/tmp"}));
}

TEST(SettingsTest, AddFavoriteDirectoryDoesNotDuplicate) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.removeFavoriteDirectory(QDir::homePath()); // drop the first-run seed
    settings.addFavoriteDirectory("/tmp");
    settings.addFavoriteDirectory("/tmp");
    EXPECT_EQ(settings.favoriteDirectories().size(), 1);
}

TEST(SettingsTest, RemoveFavoriteDirectoryRemovesIt) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.removeFavoriteDirectory(QDir::homePath()); // drop the first-run seed
    settings.addFavoriteDirectory("/tmp");
    settings.addFavoriteDirectory("/home");
    settings.removeFavoriteDirectory("/tmp");
    EXPECT_EQ(settings.favoriteDirectories(), QStringList({"/home"}));
}
