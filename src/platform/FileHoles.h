#pragma once

#include <QString>
#include <QtGlobal>

namespace fc {

// Whether a local file has nothing but zeros where a player expected data.
//
// An unfinished download keeps its full size: the downloader preallocates it
// and fills pieces in as they arrive, so a file that is 2% present looks 100%
// complete to every size check, opens fine, and plays until it reaches the
// part that was never written. Measured on one such file -- 2.16 GiB, of which
// 240 evenly spaced 4 KiB samples found data in 5 -- the video ran out at 134 s
// of a 2h44m film.
//
// Reading a hole is close to free (measured 0-1 ms per MiB against 86 ms for
// real data, because the pages are never fetched from the disk at all), so
// this can be asked at the moment something goes wrong without adding a wait
// to the failure the user is already looking at.
struct HoleReport {
    bool sampled = false;   // false when the question could not be asked
    bool aroundOffset = false; // the bytes at the offset asked about are zeros
    int zeroSamples = 0;
    int totalSamples = 0;

    // Most of the file is not there. This is deliberately a property of the
    // FILE and not of the offset: mapping a playback time to a byte offset by
    // interpolation is off by 30% on real media (measured -- 134 s into one
    // file sits at 41 MB, and even spacing puts it at 32 MB), so requiring the
    // offset probe to agree would make the verdict depend on that error. The
    // question is only ever asked after playback has already failed, so the
    // false-positive risk this leaves is a healthy video file that happens to
    // be more than half zeros, which compressed media never is.
    bool looksIncomplete() const {
        return sampled && totalSamples > 0 && zeroSamples * 2 > totalSamples;
    }
};

// Samples `path` around `offset` and across the whole file. `offset` may be
// negative or past the end, in which case only the whole-file sweep is done.
// Never touches anything but a local file: a network path would pay a round
// trip per sample, which is the opposite of the point.
HoleReport inspectForHoles(const QString &path, qint64 offset);

// The byte offset a player would be reading at, for a time in a file whose
// media is laid out roughly evenly. Crude on purpose -- it decides where to
// SAMPLE, not what to decode -- and clamped inside the file.
qint64 approximateOffsetForTime(qint64 fileSize, double seconds, double durationSeconds);

} // namespace fc
