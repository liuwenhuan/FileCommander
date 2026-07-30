#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

#include "diagnostics/RuntimeCounters.h"
#include "filesystem/FileProvider.h"
#include "network/NetworkSession.h"
#include "operations/OperationQueue.h"

namespace {

class OfflineSessionProvider final : public FileProvider {
public:
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &path) const override { return path; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return false; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

bool sameSnapshot(const fc::RuntimeSnapshot &left, const fc::RuntimeSnapshot &right) {
    return left.networkSessions == right.networkSessions &&
           left.networkThreads == right.networkThreads &&
           left.activeHeartbeats == right.activeHeartbeats &&
           left.transferWorkers == right.transferWorkers &&
           left.curlTransfers == right.curlTransfers;
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

TEST(RuntimeCounters, FinalReporterDrainsOrBoundsNetworkSessionCleanup) {
    const QByteArray childMode = qgetenv("FC_RUNTIME_FINAL_REPORTER_CHILD");
    if (!childMode.isEmpty()) {
        const fc::RuntimeSnapshot baseline = fc::runtimeSnapshot();
        auto provider = std::make_shared<OfflineSessionProvider>();
        std::shared_ptr<NetworkSession> session(
            new NetworkSession(provider), [](NetworkSession *value) { value->shutdownAsync(); });

        if (childMode == QByteArrayLiteral("drain")) {
            std::atomic<bool> connected{false};
            QObject::connect(
                session.get(), &NetworkSession::stateChanged,
                [&connected](int state, int) {
                    if (state == NetworkSession::Connected)
                        connected = true;
                });
            session->start([](QString *) { return true; }, QStringLiteral("/"));
            ASSERT_TRUE(spinUntil([&] { return connected.load(); }, 3000));
            session.reset();

            std::unique_ptr<fc::RuntimeCounterGuard> delayedCounter =
                std::make_unique<fc::RuntimeCounterGuard>(fc::RuntimeCounter::CurlTransfer);
            QObject timerContext;
            QTimer::singleShot(75, &timerContext, [&delayedCounter] { delayedCounter.reset(); });

            EXPECT_TRUE(fc::reportFinalRuntimeSnapshotIfEnabled(1000));
            EXPECT_TRUE(sameSnapshot(fc::runtimeSnapshot(), baseline));
            EXPECT_FALSE(fc::reportRuntimeSnapshotIfEnabled());
            return;
        }

        if (childMode == QByteArrayLiteral("stalled")) {
            QSemaphore release;
            std::atomic<bool> entered{false};
            session->start(
                [&entered, &release](QString *) {
                    entered = true;
                    release.acquire();
                    return false;
                },
                QStringLiteral("/"));
            if (!spinUntil([&] { return entered.load(); }, 3000)) {
                release.release();
                FAIL() << "worker never entered the offline stall";
                return;
            }
            session.reset();

            QElapsedTimer elapsed;
            elapsed.start();
            EXPECT_TRUE(fc::reportFinalRuntimeSnapshotIfEnabled(75));
            EXPECT_LT(elapsed.elapsed(), 500);
            const fc::RuntimeSnapshot bounded = fc::runtimeSnapshot();
            EXPECT_EQ(bounded.networkSessions, baseline.networkSessions + 1);
            EXPECT_EQ(bounded.networkThreads, baseline.networkThreads + 1);
            EXPECT_FALSE(fc::reportRuntimeSnapshotIfEnabled());

            release.release();
            EXPECT_TRUE(spinUntil(
                [&] { return sameSnapshot(fc::runtimeSnapshot(), baseline); }, 4000));
            return;
        }

        FAIL() << "unknown child mode";
        return;
    }

    auto runChild = [](const QByteArray &mode) {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FC_RUNTIME_FINAL_REPORTER_CHILD"),
                           QString::fromLatin1(mode));
        environment.insert(QStringLiteral("FILECOMMANDER_DIAGNOSTICS"), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("--gtest_filter="
                            "RuntimeCounters.FinalReporterDrainsOrBoundsNetworkSessionCleanup")});
        const bool finished = child.waitForFinished(10000);
        if (!finished) {
            child.kill();
            child.waitForFinished(1000);
        }
        return qMakePair(finished && child.exitStatus() == QProcess::NormalExit &&
                             child.exitCode() == 0,
                         child.readAll());
    };

    const auto drained = runChild(QByteArrayLiteral("drain"));
    EXPECT_TRUE(drained.first) << drained.second.toStdString();

    const auto stalled = runChild(QByteArrayLiteral("stalled"));
    EXPECT_TRUE(stalled.first) << stalled.second.toStdString();
}
