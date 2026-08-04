#include <gtest/gtest.h>

#include <QApplication>
#include <QStandardPaths>

// The viewer suite used to be gtest_main with no QApplication, which was enough
// for OfficeConverter (a path resolver). The F4 editor tests instantiate real
// widgets and grab their painted pixels, so the suite needs a QApplication --
// and one built as a QApplication, not QCoreApplication, since QWidget requires
// it. Defaults to offscreen for headless CI, the same way tests/ui does.
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    Q_UNUSED(app);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
