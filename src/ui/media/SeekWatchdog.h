#pragma once

#include <QtGlobal>

// Watches a seek that was accepted but may never finish.
//
// Media Foundation can take a SetCurrentTime, return S_OK, report no error at
// all -- and then stay in the seeking state forever: the clock stops, no
// further frames arrive, and every later seek is silently refused. Measured on
// a 2.16 GiB file that turned out to hold only 38.9 MB of real data with the
// rest zero-filled (an unfinished download that had preallocated its full
// size): seeking anywhere inside the first ~165 s worked, anywhere past it
// wedged permanently, and 60 s of waiting produced neither an error nor a
// recovery. ffmpeg fails at exactly the same boundary, so the file is what is
// wrong -- but ffmpeg says so, and Media Foundation just stops.
//
// An incomplete or damaged file is ordinary enough that the pane has to cope
// with it, and the only signal available is the absence of progress. This
// holds that judgement, away from any Media Foundation type, so it can be
// tested with plain numbers.
class SeekWatchdog {
public:
    enum class Verdict {
        Idle,      // nothing to watch
        Waiting,   // the seek is still plausibly in progress
        Completed, // playback resumed past the target
        Stuck,     // the deadline passed with no sign of life
    };

    // How long a seek may take before it counts as wedged. A real seek on a
    // spinning USB disk measured 1.5-2.2 s worst case, so this leaves room for
    // a slow medium without making the user stare at a dead picture.
    static constexpr qint64 kTimeoutMs = 5000;

    // Only arm while playing: a paused seek legitimately leaves the clock
    // still, which is indistinguishable from the wedge this looks for.
    void arm(double targetSeconds, double durationSeconds, bool playing, qint64 nowMs);
    void disarm();

    bool armed() const { return m_armed; }
    double target() const { return m_target; }

    Verdict observe(double positionSeconds, qint64 nowMs);

private:
    bool m_armed = false;
    double m_target = 0.0;
    double m_duration = 0.0;
    qint64 m_armedAtMs = 0;
};
