#include <gtest/gtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "DirectoryChangeMonitor.h"

namespace {

bool waitForSignal(QSignalSpy &spy, int timeoutMs = 2500) {
    return spy.count() > 0 || spy.wait(timeoutMs);
}

} // namespace

TEST(DirectoryChangeMonitorTest, ExternalCreateIsReported) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    DirectoryChangeMonitor monitor;
    QSignalSpy changed(&monitor, &DirectoryChangeMonitor::changesDetected);
    ASSERT_TRUE(monitor.startWatching(dir.path()));

    QFile file(dir.filePath(QStringLiteral("created.txt")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("x"), 1);
    file.close();

    EXPECT_TRUE(waitForSignal(changed));
    EXPECT_EQ(monitor.state(), DirectoryChangeMonitor::State::Watching);
}

TEST(DirectoryChangeMonitorTest, InvalidDirectoryRequestsReconciliation) {
    DirectoryChangeMonitor monitor;
    QSignalSpy reconcile(&monitor, &DirectoryChangeMonitor::reconciliationRequired);

    EXPECT_FALSE(monitor.startWatching(QStringLiteral("Z:/filecommander-no-such-dir")));
    EXPECT_EQ(monitor.state(), DirectoryChangeMonitor::State::NeedsReconciliation);
    EXPECT_EQ(reconcile.count(), 1);
}

TEST(DirectoryChangeMonitorTest, StopDisablesFurtherNotifications) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    DirectoryChangeMonitor monitor;
    QSignalSpy changed(&monitor, &DirectoryChangeMonitor::changesDetected);
    ASSERT_TRUE(monitor.startWatching(dir.path()));
    monitor.stopWatching();

    QFile file(dir.filePath(QStringLiteral("after-stop.txt")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    QTest::qWait(100);

    EXPECT_EQ(monitor.state(), DirectoryChangeMonitor::State::Stopped);
    EXPECT_EQ(changed.count(), 0);
}

