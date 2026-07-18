#include <gtest/gtest.h>

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

TEST(SettingsTest, WindowGeometryRoundTrips) {
    IsolatedConfigDir isolated;
    Settings settings;
    const QByteArray blob = "fake-geometry-bytes";
    settings.setWindowGeometry(blob);
    EXPECT_EQ(settings.windowGeometry(), blob);
}
