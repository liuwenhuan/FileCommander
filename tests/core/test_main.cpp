// Custom entry point (replacing gtest_main) because IconCache pulls in
// QFileIconProvider/QIcon, which need a QApplication/QPA platform to be
// initialized before use. Forces the offscreen platform so these tests
// run the same in CI (no display) as they do locally.
#include <gtest/gtest.h>

#include "CrashReporter.h"

#include <QApplication>

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // Any crash from here on names the test it happened in and prints a stack.
    // Without it a crash in this binary left an exit code and nothing else --
    // which is exactly what one intermittent access violation did.
    fc::installCrashReporter();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
