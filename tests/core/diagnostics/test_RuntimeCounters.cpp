#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include "diagnostics/RuntimeCounters.h"
#include "operations/OperationQueue.h"

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
    if (qEnvironmentVariableIntValue("FC_RUNTIME_REPORTER_CHILD") == 1) {
        const bool enabled = qEnvironmentVariableIntValue("FILECOMMANDER_DIAGNOSTICS") == 1;
        EXPECT_EQ(fc::reportRuntimeSnapshotIfEnabled(), enabled);
        EXPECT_FALSE(fc::reportRuntimeSnapshotIfEnabled());
        return;
    }

    auto runChild = [](bool diagnosticsEnabled) {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FC_RUNTIME_REPORTER_CHILD"), QStringLiteral("1"));
        if (diagnosticsEnabled)
            environment.insert(QStringLiteral("FILECOMMANDER_DIAGNOSTICS"), QStringLiteral("1"));
        else
            environment.remove(QStringLiteral("FILECOMMANDER_DIAGNOSTICS"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--gtest_filter="
                                    "RuntimeCounters.DiagnosticsReporterIsOptInAndEmitsOnlyOnce")});
        EXPECT_TRUE(child.waitForFinished(10000));
        EXPECT_EQ(child.exitStatus(), QProcess::NormalExit);
        EXPECT_EQ(child.exitCode(), 0);
        return child.exitCode();
    };

    EXPECT_EQ(runChild(false), 0);
    EXPECT_EQ(runChild(true), 0);
}
