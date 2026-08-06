#pragma once

// A wall-clock budget, asserted only where the number means something.
//
// An unoptimised build is several times slower than the one users run, so a
// millisecond budget measured against it is measuring the compiler. Same
// machine, same test, same 512 KB file rendered as hex:
//
//     Release  283 ms
//     Debug   1500 ms   against a 1000 ms budget
//
// The budget is right and the code is fast; the build was the wrong one to
// judge it in. Asserting anyway trains everyone to ignore a red suite, which
// costs more than the check is worth.
//
// So: enforced in optimised builds, and in unoptimised ones recorded as a gtest
// property instead, where it is visible to anyone looking without failing the
// run. Deliberately NOT a wider budget for debug -- a number nobody can justify
// is worse than no number.

#include <gtest/gtest.h>

#include <string>

namespace fc {

// True where a timing figure is worth asserting on.
constexpr bool timingBudgetsAreMeaningful() {
#if defined(NDEBUG)
    return true;
#else
    return false;
#endif
}

} // namespace fc

// Fails if `elapsedMs` exceeds `budgetMs`, in optimised builds only.
#define FC_EXPECT_WITHIN_BUDGET(elapsedMs, budgetMs, what)                                         \
    do {                                                                                           \
        const long long fcElapsed = static_cast<long long>(elapsedMs);                             \
        ::testing::Test::RecordProperty(what "_ms", std::to_string(fcElapsed));                    \
        if (::fc::timingBudgetsAreMeaningful()) {                                                  \
            EXPECT_LT(fcElapsed, static_cast<long long>(budgetMs))                                 \
                << what << " took " << fcElapsed << " ms against a " << (budgetMs)                 \
                << " ms budget";                                                                   \
        } else {                                                                                   \
            GTEST_LOG_(INFO) << what << ": " << fcElapsed << " ms (budget " << (budgetMs)          \
                             << " ms, not enforced in an unoptimised build)";                      \
        }                                                                                          \
    } while (false)
