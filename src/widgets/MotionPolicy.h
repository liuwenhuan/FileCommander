#pragma once

#include <QEasingCurve>

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

    // The application wires the persisted user preference into this policy.
    static void setApplicationReduced(bool reduced);

    // Deterministic tests may override all runtime reduction sources.
    static void setReducedForTest(bool reduced);
    static void clearReducedForTest();
};
