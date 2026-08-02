#include "MotionPolicy.h"

#include <QByteArray>
#include <QGlobalStatic>
#include <QObject>

#include <atomic>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

std::atomic<int> reducedForTest{-1};
std::atomic<int> systemReducedForTest{-1};

class MotionPolicyNotifier : public QObject {
    Q_OBJECT

public:
    void publish(bool reduced) { emit reducedChanged(reduced); }

signals:
    void reducedChanged(bool reduced);
};

Q_GLOBAL_STATIC(MotionPolicyNotifier, motionPolicyNotifier)

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

void publishReducedChange(bool wasReduced) {
    const bool isReduced = MotionPolicy::reduced();
    if (isReduced != wasReduced)
        motionPolicyNotifier()->publish(isReduced);
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

    return systemReducesMotion();
}

bool MotionPolicy::allowFor(InputCadence cadence) {
    return cadence == InputCadence::Normal && !reduced();
}

void MotionPolicy::observeReduced(QObject *context, std::function<void(bool)> observer) {
    QObject::connect(motionPolicyNotifier(), &MotionPolicyNotifier::reducedChanged, context,
                     std::move(observer));
}

void MotionPolicy::setReducedForTest(bool reduced) {
    const bool wasReduced = MotionPolicy::reduced();
    reducedForTest.store(reduced ? 1 : 0);
    publishReducedChange(wasReduced);
}

void MotionPolicy::clearReducedForTest() {
    const bool wasReduced = MotionPolicy::reduced();
    reducedForTest.store(-1);
    publishReducedChange(wasReduced);
}

void MotionPolicy::setSystemReducedForTest(bool reduced) {
    const bool wasReduced = MotionPolicy::reduced();
    systemReducedForTest.store(reduced ? 1 : 0);
    publishReducedChange(wasReduced);
}

void MotionPolicy::clearSystemReducedForTest() {
    const bool wasReduced = MotionPolicy::reduced();
    systemReducedForTest.store(-1);
    publishReducedChange(wasReduced);
}

#include "MotionPolicy.moc"
