#include "network/ConnectionPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

struct FakeConn {};

bool waitFor(const std::atomic_int &value, int expected, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load() == expected;
}

TEST(ConnectionPoolTest, ShutdownAsyncReturnsBeforeSlowIdleDestroyerFinishes) {
    std::atomic_int destroyed{0};
    ConnectionPool<FakeConn> pool;
    pool.configure(
        [](QString *) { return new FakeConn; },
        [&destroyed](FakeConn *conn) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++destroyed;
            delete conn;
        },
        1);

    FakeConn *conn = pool.borrow(nullptr);
    ASSERT_NE(conn, nullptr);
    pool.release(conn);

    const auto started = std::chrono::steady_clock::now();
    pool.shutdownAsync();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_LT(elapsed.count(), 50);
    EXPECT_TRUE(waitFor(destroyed, 1, 500));
}

TEST(ConnectionPoolTest, ReleaseAfterShutdownDestroysBorrowedConnection) {
    std::atomic_int destroyed{0};
    ConnectionPool<FakeConn> pool;
    pool.configure(
        [](QString *) { return new FakeConn; },
        [&destroyed](FakeConn *conn) {
            ++destroyed;
            delete conn;
        },
        1);

    FakeConn *conn = pool.borrow(nullptr);
    ASSERT_NE(conn, nullptr);
    pool.shutdownAsync();
    pool.release(conn);

    EXPECT_EQ(destroyed.load(), 1);
}

TEST(ConnectionPoolTest, DrainRejectsLateReapersWithoutStartingDestroyer) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        std::atomic_int destroyed{0};
        ConnectionPool<FakeConn> pool;
        pool.configure(
            [](QString *) { return new FakeConn; },
            [&destroyed](FakeConn *conn) {
                ++destroyed;
                delete conn;
            },
            1);
        FakeConn *conn = pool.borrow(nullptr);
        if (!conn || !connpool::ReaperRegistry::instance().drain(0))
            _exit(2);
        pool.release(conn);
        pool.shutdownAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const char result = static_cast<char>(destroyed.load());
        const bool wrote = write(pipefd[1], &result, sizeof(result)) == sizeof(result);
        close(pipefd[1]);
        _exit(wrote ? 0 : 3);
    }

    close(pipefd[1]);
    char result = -1;
    ASSERT_EQ(read(pipefd[0], &result, sizeof(result)), sizeof(result));
    close(pipefd[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(result, 0);
}

TEST(ConnectionPoolTest, ThrowingDestroyerLeavesRegistryDrained) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        ConnectionPool<FakeConn> pool;
        pool.configure(
            [](QString *) { return new FakeConn; },
            [](FakeConn *conn) {
                delete conn;
                throw 1;
            },
            1);
        FakeConn *conn = pool.borrow(nullptr);
        if (!conn)
            _exit(2);
        pool.release(conn);
        pool.shutdownAsync();
        const char result = connpool::ReaperRegistry::instance().drain(250) ? 1 : 0;
        const bool wrote = write(pipefd[1], &result, sizeof(result)) == sizeof(result);
        close(pipefd[1]);
        _exit(wrote ? 0 : 3);
    }

    close(pipefd[1]);
    char result = 0;
    ASSERT_EQ(read(pipefd[0], &result, sizeof(result)), sizeof(result));
    close(pipefd[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(result, 1);
}

} // namespace
