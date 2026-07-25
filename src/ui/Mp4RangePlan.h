#pragma once

#include <QByteArray>
#include <QPair>
#include <QVector>

// Works out which byte ranges of an MP4/MOV a thumbnail actually needs, by
// reading the container's own structure instead of guessing a fixed budget.
//
// The file is a chain of boxes, each headed by a 32-bit size and a 4-character
// type, laid end to end. The chain is forward-linked, so a small read of the
// head names every top-level box and says exactly where the next one starts --
// including the index (moov), wherever it lives:
//
//   * written for streaming, the index sits near the front and the plan is
//     simply "everything up to the end of moov, plus a little frame data";
//   * written by ffmpeg's default muxer, the index sits at the very end, and
//     the head still yields its offset (it is whatever follows the media data,
//     whose length the chain declares). One tiny read at that offset gives the
//     index's real size, and only then do we know what to fetch.
//
// Why this matters over a fixed head/tail budget: an index grows with a
// video's length, so a fixed budget is wrong in both directions. Measured on
// real files, a 3.6 GB feature carries an 8.6 MB index -- a 2 MB head cannot
// decode it at all -- while a 428 MB clip needs only 380 KB in total. Sizes
// come from the container, so both cases work and neither over-fetches.
namespace Mp4RangePlan {

// A byte range: (offset, length).
using Range = QPair<qint64, qint64>;

struct Plan {
    // Ranges to fetch. Empty when the head was not a usable box chain, in which
    // case the caller should fall back to its generic strategy.
    QVector<Range> ranges;

    // Set when the index was not inside `head` and its size is still unknown.
    // The caller must read `probeLength` bytes at `probeOffset` and pass them
    // back through refine() to complete the plan.
    bool needsProbe = false;
    qint64 probeOffset = 0;
    qint64 probeLength = 0;
};

// Builds a plan from the first bytes of the file. `fileSize` is the real
// length, needed to bound the chain and to resolve a box declared as running to
// end-of-file.
Plan plan(const QByteArray &head, qint64 fileSize);

// Completes a plan that asked for a probe. `probe` is the bytes read at
// Plan::probeOffset. Returns the finished plan; its ranges are empty if the
// probe did not reveal a valid index there.
Plan refine(const Plan &pending, const QByteArray &probe, qint64 fileSize);

} // namespace Mp4RangePlan
