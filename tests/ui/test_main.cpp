#include <gtest/gtest.h>

#include <QApplication>

// The UI suite needs a QApplication rather than gtest's bare main(): the
// thumbnail tests build QPixmaps and rely on queued invocations reaching the
// main thread, both of which require a running Qt application object. Offscreen
// so the suite runs headless in CI with no X display.
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
