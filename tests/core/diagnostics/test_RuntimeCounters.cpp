#include <gtest/gtest.h>

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

TEST(RuntimeCounters, OperationQueueTracksEveryTransferWorker) {
    const fc::RuntimeSnapshot before = fc::runtimeSnapshot();
    {
        OperationQueue queue;
        EXPECT_EQ(fc::runtimeSnapshot().transferWorkers,
                  before.transferWorkers + queue.maxConcurrentTransfers());
    }
    EXPECT_EQ(fc::runtimeSnapshot().transferWorkers, before.transferWorkers);
}
