#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "DirectoryChangeMonitor.h"
#include "FileSystemModel.h"

namespace {

bool containsName(const FileSystemModel &model, const QString &name) {
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.fileInfoAt(row).name() == name)
            return true;
    return false;
}

bool waitForName(const FileSystemModel &model, const QString &name, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (containsName(model, name))
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(20);
    }
    return containsName(model, name);
}

} // namespace

TEST(FileSystemModelExternalRefreshTest, ExternalCreateIsRelistedAndShowsNewRow) {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
    GTEST_SKIP() << "No native directory notification backend on this platform";
#else
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    FileSystemModel model;
    QSignalSpy finished(&model, &FileSystemModel::loadFinished);
    model.setRootPath(dir.path());
    ASSERT_TRUE(finished.wait(3000) || finished.count() > 0);

    auto *monitor = model.findChild<DirectoryChangeMonitor *>();
    ASSERT_NE(monitor, nullptr);
    ASSERT_TRUE(monitor->isWatching());

    const QString filePath = dir.filePath(QStringLiteral("created-after-load.txt"));
    QFile file(filePath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("external");
    file.close();

    EXPECT_TRUE(waitForName(model, QStringLiteral("created-after-load.txt"), 4000));
#endif
}
