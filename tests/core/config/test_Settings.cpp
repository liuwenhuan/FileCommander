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

TEST(SettingsTest, ExplicitIniPathPersistsWithoutChangingGlobalConfigLocation) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString settingsPath = temporaryDir.filePath(QStringLiteral("isolated.ini"));
    Settings settings(settingsPath);
    settings.setNotepadEditorHeight(180);

    Settings reloaded(settingsPath);
    EXPECT_EQ(reloaded.notepadEditorHeight(), 180);
}

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

TEST(SettingsTest, ListFontSizeDefaultsToTwelve) {
    IsolatedConfigDir isolated;
    Settings settings;
    EXPECT_EQ(settings.listFontSize(), 12);
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
    EXPECT_EQ(settings.listFontSize(), 8);
    settings.setListFontSize(99);
    EXPECT_EQ(settings.listFontSize(), 18);
}

TEST(SettingsTest, InterfacePreferencesHaveCompatibleDefaultsAndRoundTrip) {
    IsolatedConfigDir isolated;
    Settings settings;

    EXPECT_TRUE(settings.listFontFamily().isEmpty());
    EXPECT_TRUE(settings.showTabBar());
    EXPECT_TRUE(settings.showShortcutLabels());
    EXPECT_TRUE(settings.autoUpdateCheck());

    settings.setListFontFamily(QStringLiteral("Noto Sans CJK SC"));
    settings.setShowTabBar(false);
    settings.setShowShortcutLabels(false);
    settings.setAutoUpdateCheck(false);

    EXPECT_EQ(settings.listFontFamily(), QStringLiteral("Noto Sans CJK SC"));
    EXPECT_FALSE(settings.showTabBar());
    EXPECT_FALSE(settings.showShortcutLabels());
    EXPECT_FALSE(settings.autoUpdateCheck());
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

TEST(SettingsTest, MaxConcurrentTransfersDefaultsToTwo) {
    IsolatedConfigDir isolated;
    Settings settings;
    EXPECT_EQ(settings.maxConcurrentTransfers(), 2);
}

TEST(SettingsTest, MaxConcurrentTransfersRoundTrips) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.setMaxConcurrentTransfers(4);
    EXPECT_EQ(settings.maxConcurrentTransfers(), 4);
}

TEST(SettingsTest, MaxConcurrentTransfersClampsToRange) {
    IsolatedConfigDir isolated;
    Settings settings;
    settings.setMaxConcurrentTransfers(0);
    EXPECT_EQ(settings.maxConcurrentTransfers(), 1);
    settings.setMaxConcurrentTransfers(99);
    EXPECT_EQ(settings.maxConcurrentTransfers(), 8);
}

TEST(SettingsTest, VideoMutedDefaultsToFalse) {
    IsolatedConfigDir iso;
    Settings settings;
    EXPECT_FALSE(settings.videoMuted())
        << "A fresh install should preview with sound — the user unmutes once to discover it";
}

TEST(SettingsTest, VideoMutedRoundTripsExplicitTrue) {
    IsolatedConfigDir iso;
    Settings settings;
    settings.setVideoMuted(true);
    EXPECT_TRUE(settings.videoMuted()) << "Explicitly muted must survive reload";
    Settings reloaded;
    EXPECT_TRUE(reloaded.videoMuted()) << "Persisted mute must survive reload";
}

TEST(SettingsTest, VideoMutedRoundTripsExplicitFalse) {
    IsolatedConfigDir iso;
    Settings settings;
    settings.setVideoMuted(true);  // from fresh default false
    settings.setVideoMuted(false);
    EXPECT_FALSE(settings.videoMuted());
    Settings reloaded;
    EXPECT_FALSE(reloaded.videoMuted());
}

TEST(SettingsTest, VideoVolumeDefaultsToSeventy) {
    IsolatedConfigDir iso;
    Settings settings;
    EXPECT_EQ(settings.videoVolume(), 70);
}

TEST(SettingsTest, VideoVolumeClamps) {
    IsolatedConfigDir iso;
    Settings settings;
    settings.setVideoVolume(-5);
    EXPECT_EQ(settings.videoVolume(), 0);
    settings.setVideoVolume(200);
    EXPECT_EQ(settings.videoVolume(), 100);
}
