#include "MotionPolicy.h"

#include <QByteArray>

#include <atomic>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

std::atomic<int> reducedForTest{-1};
std::atomic<int> systemReducedForTest{-1};
std::atomic_bool applicationReduced{false};

bool systemReducesMotion() {
    const int testOverride = systemReducedForTest.load();
    if (testOverride >= 0)
        return testOverride != 0;

#if defined(Q_OS_WIN)
    BOOL clientAreaAnimation = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAreaAnimation, 0) &&
           !clientAreaAnimation;
#else
    return false;
#endif
}

} // namespace

int MotionPolicy::duration(MotionDuration motionDuration) {
    if (reduced())
        return 0;

    switch (motionDuration) {
    case MotionDuration::Fast:
        return 100;
    case MotionDuration::Normal:
        return 150;
    case MotionDuration::Slow:
        return 200;
    }
    return 0;
}

QEasingCurve MotionPolicy::easing() {
    return QEasingCurve(QEasingCurve::OutCubic);
}

bool MotionPolicy::reduced() {
    const int testOverride = reducedForTest.load();
    if (testOverride >= 0)
        return testOverride != 0;

    if (qgetenv("FILECOMMANDER_DISABLE_ANIMATIONS") == QByteArrayLiteral("1"))
        return true;

    return applicationReduced.load() || systemReducesMotion();
}

bool MotionPolicy::allowFor(InputCadence cadence) {
    return cadence == InputCadence::Normal && !reduced();
}

void MotionPolicy::setApplicationReduced(bool reduced) {
    applicationReduced.store(reduced);
}

void MotionPolicy::setReducedForTest(bool reduced) {
    reducedForTest.store(reduced ? 1 : 0);
}

void MotionPolicy::clearReducedForTest() {
    reducedForTest.store(-1);
}

void MotionPolicy::setSystemReducedForTest(bool reduced) {
    systemReducedForTest.store(reduced ? 1 : 0);
}

void MotionPolicy::clearSystemReducedForTest() {
    systemReducedForTest.store(-1);
}
