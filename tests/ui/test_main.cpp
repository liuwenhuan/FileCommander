#include <gtest/gtest.h>

#include <QApplication>
#include <QStandardPaths>

// The suite defaults to offscreen for headless CI. A caller can explicitly set
// QT_QPA_PLATFORM=xcb (with DISPLAY=:1) for real X11 geometry and theme tests.
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Every path QStandardPaths hands out moves to a throwaway test location for
    // the whole run. The thumbnail tests write to, count, and now DELETE files
    // in the cache directory; against the real one a test run would quietly
    // destroy the developer's own thumbnail cache (and fight any second process
    // sharing it). Set before the first test so it also covers the
    // ThumbnailCache singleton's construction, which touches the cache
    // directory on its own.
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
