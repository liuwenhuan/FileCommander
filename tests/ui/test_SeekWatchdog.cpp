#include <gtest/gtest.h>

#include "media/SeekWatchdog.h"

// The defect this guards against, measured on
// D:/usbhdd/<a 2.16 GiB, 2h44m H.264 file>: SetCurrentTime(4921.3) returns
// S_OK, GetCurrentTime() reports 4921.3 -- and then nothing ever moves again.
// IsSeeking() stays true for at least 60 s, no error event arrives, and every
// later seek is refused silently. So "the call succeeded" and "the position
// equals the target" are both true in the failure case, and neither can be
// used as the completion signal.

TEST(SeekWatchdogTest, IdleUntilArmed) {
    SeekWatchdog watchdog;
    EXPECT_FALSE(watchdog.armed());
    EXPECT_EQ(watchdog.observe(12.0, 100), SeekWatchdog::Verdict::Idle);
}

TEST(SeekWatchdogTest, CompletesWhenPlaybackMovesPastTheTarget) {
    SeekWatchdog watchdog;
    watchdog.arm(4921.3, 9842.6, true, 0);
    ASSERT_TRUE(watchdog.armed());
    EXPECT_EQ(watchdog.observe(4921.3, 100), SeekWatchdog::Verdict::Waiting);
    EXPECT_EQ(watchdog.observe(4921.3, 2000), SeekWatchdog::Verdict::Waiting);
    EXPECT_EQ(watchdog.observe(4922.0, 2100), SeekWatchdog::Verdict::Completed);
    EXPECT_FALSE(watchdog.armed());
}

// Landing on the target is exactly what the wedged engine reports, so it must
// not count as success on its own.
TEST(SeekWatchdogTest, SittingOnTheTargetIsTheFailureItLooksFor) {
    SeekWatchdog watchdog;
    watchdog.arm(4921.3, 9842.6, true, 0);
    EXPECT_EQ(watchdog.observe(4921.3, SeekWatchdog::kTimeoutMs - 1),
              SeekWatchdog::Verdict::Waiting);
    EXPECT_EQ(watchdog.observe(4921.3, SeekWatchdog::kTimeoutMs), SeekWatchdog::Verdict::Stuck);
    EXPECT_FALSE(watchdog.armed());
}

// One verdict per seek: after Stuck the engine reloads, and a second Stuck for
// the same seek would reload again on top of that.
TEST(SeekWatchdogTest, ReportsStuckOnlyOnce) {
    SeekWatchdog watchdog;
    watchdog.arm(100.0, 9842.6, true, 0);
    ASSERT_EQ(watchdog.observe(100.0, SeekWatchdog::kTimeoutMs), SeekWatchdog::Verdict::Stuck);
    EXPECT_EQ(watchdog.observe(100.0, SeekWatchdog::kTimeoutMs + 5000),
              SeekWatchdog::Verdict::Idle);
}

// Seeking to the very end leaves nothing to play, so the clock cannot advance
// past the target and the "no progress" rule would fire on a healthy seek.
TEST(SeekWatchdogTest, ATargetAtTheEndCompletesOnArrival) {
    SeekWatchdog watchdog;
    watchdog.arm(9842.4, 9842.6, true, 0);
    EXPECT_EQ(watchdog.observe(9842.4, SeekWatchdog::kTimeoutMs + 1000),
              SeekWatchdog::Verdict::Completed);
}

// A paused clip legitimately has a still clock; arming there would report every
// paused seek as wedged.
TEST(SeekWatchdogTest, DoesNotArmWhilePaused) {
    SeekWatchdog watchdog;
    watchdog.arm(4921.3, 9842.6, false, 0);
    EXPECT_FALSE(watchdog.armed());
    EXPECT_EQ(watchdog.observe(4921.3, SeekWatchdog::kTimeoutMs * 10), SeekWatchdog::Verdict::Idle);
}

// Loading another clip must cancel a seek still being watched, or the new clip
// gets reloaded out from under the user.
TEST(SeekWatchdogTest, DisarmCancelsTheWatch) {
    SeekWatchdog watchdog;
    watchdog.arm(4921.3, 9842.6, true, 0);
    watchdog.disarm();
    EXPECT_EQ(watchdog.observe(0.0, SeekWatchdog::kTimeoutMs * 10), SeekWatchdog::Verdict::Idle);
}

// A slow but working seek -- 2.2 s was the worst measured on a USB disk -- must
// not be cut short.
TEST(SeekWatchdogTest, ASlowButRealSeekIsNotCutShort) {
    SeekWatchdog watchdog;
    watchdog.arm(500.0, 9842.6, true, 0);
    EXPECT_EQ(watchdog.observe(500.0, 2200), SeekWatchdog::Verdict::Waiting);
    EXPECT_EQ(watchdog.observe(500.9, 2300), SeekWatchdog::Verdict::Completed);
}
