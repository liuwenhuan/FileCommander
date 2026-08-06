#pragma once

// Waiting for a condition, in a way that fails when the condition never comes.
//
// Qt's QTRY_VERIFY_WITH_TIMEOUT and friends do NOT do that inside a gtest TEST.
// They expand to QVERIFY, which on failure calls `return` -- and a gtest body
// returns void, so the macro simply leaves the test early and the test is
// reported as passing. Measured, not assumed: a TEST containing
//
//     QTRY_VERIFY_WITH_TIMEOUT(false, 200);
//     ADD_FAILURE() << "reached the line after a QTRY that timed out";
//
// reports [ OK ] after 661 ms, and the ADD_FAILURE never runs.
//
// That left 72 assertions across 15 files unable to fail. Where one of them was
// the last real check in a test, the test verified nothing at all.
//
// These wait the same way and then assert for real. Same shape as the Qt
// macros, so a call site converts by changing the name.
//
// Written as plain loops rather than around a lambda: some of these live in
// template functions, and MSVC would not compile the lambda form there --
// "'acceptedFinal': undeclared identifier" for a local that was declared eight
// lines above.
//
// The spin uses QTest::qWait rather than processEvents. processEvents returns
// the moment the queue is empty, so the loop became a busy wait that burned a
// core and starved the very animation timer it was waiting on -- a drag
// feedback animation then failed to advance 20 ms inside a two-second budget.
// qWait sleeps between passes, which is what lets the thing being waited for
// actually get to run.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QTest>

// Fails the test if `expr` has not become true within `timeoutMs`.
#define FC_TRY_VERIFY_WITH_TIMEOUT(expr, timeoutMs)                                                \
    do {                                                                                           \
        QElapsedTimer fcTryTimer;                                                                  \
        fcTryTimer.start();                                                                        \
        while (!(expr) && fcTryTimer.elapsed() < (timeoutMs))                                      \
            QTest::qWait(5);                                                                       \
        ASSERT_TRUE(expr) << "timed out after " << (timeoutMs) << " ms waiting for: " #expr;       \
    } while (false)

// Fails the test if `actual` has not become equal to `expected` in time, and
// says what it was instead.
#define FC_TRY_COMPARE_WITH_TIMEOUT(actual, expected, timeoutMs)                                   \
    do {                                                                                           \
        QElapsedTimer fcTryTimer;                                                                  \
        fcTryTimer.start();                                                                        \
        while (!((actual) == (expected)) && fcTryTimer.elapsed() < (timeoutMs))                    \
            QTest::qWait(5);                                                                       \
        ASSERT_EQ((actual), (expected)) << "still unequal after " << (timeoutMs) << " ms";         \
    } while (false)
