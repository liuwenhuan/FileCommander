#include <gtest/gtest.h>

#include <QDebug>

#include <atomic>

#include "diagnostics/RuntimeCounters.h"
#include "operations/OperationQueue.h"

namespace {

std::atomic<int> runtimeSnapshotMessages{0};

void countRuntimeSnapshotMessages(QtMsgType, const QMessageLogContext &, const QString &message) {
    if (message.startsWith(QStringLiteral("FileCommander RuntimeSnapshot")))
        ++runtimeSnapshotMessages;
}

} // namespace

TEST(RuntimeCounters, GuardTracksLifetime) {
    const fc::RuntimeSnapshot before = fc::runtimeSnapshot();
    {
        fc::RuntimeCounterGuard guard(fc::RuntimeCounter::CurlTransfer);
        EXPECT_EQ(fc::runtimeSnapshot().curlTransfers, before.curlTransfers + 1);
    }
    EXPECT_EQ(fc::runtimeSnapshot().curlTransfers, before.curlTransfers);
}

TEST(RuntimeCounters, MoveTransfersTheSingleOwnedIncrement) {
    const fc::RuntimeSnapshot before = fc::runtimeSnapshot();
    {
        fc::RuntimeCounterGuard first(fc::RuntimeCounter::TransferWorker);
        fc::RuntimeCounterGuard second(std::move(first));
        EXPECT_EQ(fc::runtimeSnapshot().transferWorkers, before.transferWorkers + 1);
    }
    EXPECT_EQ(fc::runtimeSnapshot().transferWorkers, before.transferWorkers);
}

TEST(RuntimeCounters, MoveAssignmentReleasesThePreviousIncrement) {
    const fc::RuntimeSnapshot before = fc::runtimeSnapshot();
    {
        fc::RuntimeCounterGuard first(fc::RuntimeCounter::TransferWorker);
        fc::RuntimeCounterGuard second(fc::RuntimeCounter::TransferWorker);
        second = std::move(first);
        EXPECT_EQ(fc::runtimeSnapshot().transferWorkers, before.transferWorkers + 1);
    }
    EXPECT_EQ(fc::runtimeSnapshot().transferWorkers, before.transferWorkers);
}

TEST(RuntimeCounters, OperationQueueTracksEveryTransferWorker) {
    const fc::RuntimeSnapshot before = fc::runtimeSnapshot();
    {
        OperationQueue queue;
        EXPECT_EQ(fc::runtimeSnapshot().transferWorkers,
                  before.transferWorkers + queue.maxConcurrentTransfers());
    }
    EXPECT_EQ(fc::runtimeSnapshot().transferWorkers, before.transferWorkers);
}

TEST(RuntimeCounters, DiagnosticsReporterIsOptInAndEmitsOnlyOnce) {
    const QByteArray oldValue = qgetenv("FILECOMMANDER_DIAGNOSTICS");
    qunsetenv("FILECOMMANDER_DIAGNOSTICS");
    runtimeSnapshotMessages = 0;
    QtMessageHandler previous = qInstallMessageHandler(countRuntimeSnapshotMessages);

    fc::reportRuntimeSnapshotIfEnabled();
    EXPECT_EQ(runtimeSnapshotMessages.load(), 0);

    qputenv("FILECOMMANDER_DIAGNOSTICS", "1");
    fc::reportRuntimeSnapshotIfEnabled();
    fc::reportRuntimeSnapshotIfEnabled();
    EXPECT_EQ(runtimeSnapshotMessages.load(), 1);

    qInstallMessageHandler(previous);
    if (oldValue.isNull())
        qunsetenv("FILECOMMANDER_DIAGNOSTICS");
    else
        qputenv("FILECOMMANDER_DIAGNOSTICS", oldValue);
}
