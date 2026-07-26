#include "network/ConnectionPool.h"
#include "network/SmbClientGate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

bool waitFor(const std::atomic_bool &value, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load();
}

TEST(SmbClientGateTest, SerializesConcurrentCallers) {
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    bool firstEntered = false;
    bool releaseFirst = false;
    std::atomic_bool secondEntered{false};
    std::atomic_int active{0};
    std::atomic_int peakActive{0};

    std::thread first([&] {
        QMutexLocker locker(&SmbClientGate::mutex());
        const int nowActive = ++active;
        peakActive.store(qMax(peakActive.load(), nowActive));
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            firstEntered = true;
        }
        stateChanged.notify_one();

        std::unique_lock<std::mutex> stateLock(stateMutex);
        stateChanged.wait(stateLock, [&] { return releaseFirst; });
        --active;
    });

    {
        std::unique_lock<std::mutex> stateLock(stateMutex);
        ASSERT_TRUE(stateChanged.wait_for(stateLock, std::chrono::seconds(1), [&] { return firstEntered; }));
    }

    std::thread second([&] {
        QMutexLocker locker(&SmbClientGate::mutex());
        const int nowActive = ++active;
        peakActive.store(qMax(peakActive.load(), nowActive));
        secondEntered = true;
        --active;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_FALSE(secondEntered.load());

    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        releaseFirst = true;
    }
    stateChanged.notify_one();

    first.join();
    second.join();
    EXPECT_TRUE(secondEntered.load());
    EXPECT_EQ(peakActive.load(), 1);
}

TEST(SmbClientGateTest, ReaperDestroyerWaitsForForegroundCaller) {
    struct FakeContext {};

    std::atomic_bool destroyerStarted{false};
    std::atomic_bool destroyed{false};
    std::atomic_bool destroyerFinished{false};
    ConnectionPool<FakeContext> pool;
    pool.configure(
        [](QString *) { return new FakeContext; },
        [&](FakeContext *context) {
            destroyerStarted = true;
            QMutexLocker locker(&SmbClientGate::mutex());
            destroyed = true;
            delete context;
            destroyerFinished = true;
        },
        1);

    FakeContext *context = pool.borrow(nullptr);
    ASSERT_NE(context, nullptr);
    pool.release(context);

    QMutexLocker foreground(&SmbClientGate::mutex());
    pool.shutdownAsync();
    ASSERT_TRUE(waitFor(destroyerStarted, 500));
    EXPECT_FALSE(destroyed.load());
    foreground.unlock();

    EXPECT_TRUE(waitFor(destroyerFinished, 500));
    EXPECT_TRUE(destroyed.load());
}

} // namespace
