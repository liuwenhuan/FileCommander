#include <gtest/gtest.h>

#include <QWidget>

#include <functional>

#include "WindowActivation.h"

namespace {

class RecordingActivationBackend final : public ttc::WindowActivationBackend {
public:
    void showNormal(QWidget *) override { ++showNormalCalls; }
    void raise(QWidget *) override { ++raiseCalls; }
    void activate(QWidget *) override { ++activateCalls; }
    void nativeActivate(QWidget *) override { ++nativeActivateCalls; }
    void schedule(QWidget *, int delayMs, std::function<void()> callback) override {
        delays.append(delayMs);
        callbacks.append(std::move(callback));
    }

    int showNormalCalls = 0;
    int raiseCalls = 0;
    int activateCalls = 0;
    int nativeActivateCalls = 0;
    QVector<int> delays;
    QVector<std::function<void()>> callbacks;
};

TEST(WindowActivationTest, StartupActivationRunsImmediatelyAndSchedulesRetries) {
    QWidget window;
    RecordingActivationBackend backend;

    ttc::requestWindowForeground(&window, backend);

    EXPECT_EQ(backend.showNormalCalls, 1);
    EXPECT_EQ(backend.raiseCalls, 1);
    EXPECT_EQ(backend.activateCalls, 1);
    EXPECT_EQ(backend.nativeActivateCalls, 1);
    ASSERT_EQ(backend.delays.size(), 4);
    EXPECT_EQ(backend.delays.at(0), 0);
    EXPECT_EQ(backend.delays.at(1), 150);
    EXPECT_EQ(backend.delays.at(2), 600);
    EXPECT_EQ(backend.delays.at(3), 1500);
    EXPECT_EQ(backend.callbacks.size(), 4);
}

TEST(WindowActivationTest, ScheduledRetryRepeatsFullActivationSequence) {
    QWidget window;
    RecordingActivationBackend backend;

    ttc::requestWindowForeground(&window, backend);
    ASSERT_FALSE(backend.callbacks.isEmpty());
    backend.callbacks.first()();

    EXPECT_EQ(backend.showNormalCalls, 2);
    EXPECT_EQ(backend.raiseCalls, 2);
    EXPECT_EQ(backend.activateCalls, 2);
    EXPECT_EQ(backend.nativeActivateCalls, 2);
}

TEST(WindowActivationTest, NullWindowDoesNothing) {
    RecordingActivationBackend backend;

    ttc::requestWindowForeground(nullptr, backend);

    EXPECT_EQ(backend.showNormalCalls, 0);
    EXPECT_TRUE(backend.delays.isEmpty());
}

} // namespace
