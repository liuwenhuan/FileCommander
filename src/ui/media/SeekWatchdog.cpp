#include "SeekWatchdog.h"

namespace {

// The clock has to move visibly past the target before the seek counts as
// finished: landing exactly on the target is what a wedged engine reports too
// (it snaps the reported time to the requested one and then stops).
constexpr double kResumedBy = 0.25;

// A seek into the last moments of a clip cannot demonstrate progress -- there
// is nothing left to play -- so reaching the target is accepted there.
constexpr double kEndWindow = 1.5;

} // namespace

void SeekWatchdog::arm(double targetSeconds, double durationSeconds, bool playing, qint64 nowMs) {
    if (!playing) {
        disarm();
        return;
    }
    m_armed = true;
    m_target = targetSeconds;
    m_duration = durationSeconds;
    m_armedAtMs = nowMs;
}

void SeekWatchdog::disarm() {
    m_armed = false;
    m_target = 0.0;
    m_duration = 0.0;
    m_armedAtMs = 0;
}

SeekWatchdog::Verdict SeekWatchdog::observe(double positionSeconds, qint64 nowMs) {
    if (!m_armed)
        return Verdict::Idle;

    const bool nearEnd = m_duration > 0.0 && m_target >= m_duration - kEndWindow;
    if (positionSeconds > m_target + kResumedBy ||
        (nearEnd && positionSeconds >= m_target - kResumedBy)) {
        disarm();
        return Verdict::Completed;
    }
    if (nowMs - m_armedAtMs >= kTimeoutMs) {
        disarm();
        return Verdict::Stuck;
    }
    return Verdict::Waiting;
}
