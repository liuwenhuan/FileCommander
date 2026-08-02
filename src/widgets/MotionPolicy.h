#pragma once

#include <QEasingCurve>

#include <functional>

class QObject;

enum class MotionDuration { Fast, Normal, Slow };

enum class InputCadence { Normal, Rapid };

// Centralizes the application's small motion contract without owning any
// animation objects. Callers still own their local animation and state changes.
class MotionPolicy {
public:
    static int duration(MotionDuration duration);
    static QEasingCurve easing();
    static bool reduced();
    static bool allowFor(InputCadence cadence);

    // Context-bound observers are notified synchronously whenever the effective
    // reduced-motion state changes. The connection is removed with `context`.
    static void observeReduced(QObject *context, std::function<void(bool)> observer);

    // Deterministic tests may override all runtime reduction sources.
    static void setReducedForTest(bool reduced);
    static void clearReducedForTest();

    // Lets tests isolate the host system preference without changing
    // production detection when unset.
    static void setSystemReducedForTest(bool reduced);
    static void clearSystemReducedForTest();
};
