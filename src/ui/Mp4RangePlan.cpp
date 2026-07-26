#include "Mp4RangePlan.h"

namespace {

// Media data to fetch alongside the index. The index says where the frames are
// but not what they hold, so a frame grab still reads real samples. Measured
// against the sample files, 64 KB already sufficed everywhere; this keeps a
// margin for codecs with larger keyframes while staying far below the cost of
// the index itself.
constexpr qint64 kFrameDataBytes = 256 * 1024;

// Bytes to read at the index's offset to learn its declared size. Covers a
// 64-bit size field with room to spare.
constexpr qint64 kBoxHeaderProbe = 32;

// Refuse to plan around an index larger than this. A legitimate one is tens of
// MB at most; anything beyond is a corrupt or hostile size field, and fetching
// it would defeat the point of not pulling the whole file.
constexpr qint64 kMaxIndexBytes = 64LL * 1024 * 1024;

constexpr int kBoxHeaderSize = 8;

// Bytes to take from the keyframe's own position. One keyframe plus a little
// slack: the grab decodes a single frame, so this only has to span that frame,
// not the gap to the next one.
constexpr qint64 kKeyframeWindowBytes = 2 * 1024 * 1024;

// Sanity bound on table entry counts read out of the index. A real video's
// tables run to tens of thousands of entries; a wildly larger count means a
// corrupt or hostile field, and walking it would burn time and memory.
constexpr quint32 kMaxTableEntries = 8u * 1000u * 1000u;

// Every offset below is derived from a size the file itself declares, and the
// buffer is only whatever slice of a remote file came back -- short reads and
// boxes cut off mid-way are normal, not exceptional. So the raw reads are
// bounded here, in one place: out of range reads as zero, which every caller
// already treats as a malformed box and refuses.
bool inRange(const QByteArray &data, int offset, int length) {
    return offset >= 0 && static_cast<qint64>(offset) + length <= data.size();
}

quint32 beUint32(const QByteArray &data, int offset) {
    if (!inRange(data, offset, 4))
        return 0;
    return (static_cast<quint32>(static_cast<quint8>(data[offset])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(data[offset + 3]));
}

quint64 beUint64(const QByteArray &data, int offset) {
    if (!inRange(data, offset, 8))
        return 0;
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<quint8>(data[offset + i]);
    return v;
}

struct Box {
    QByteArray type;
    qint64 offset = 0;
    qint64 size = 0;
};

// Reads the box header at `pos`. Returns false when the header is truncated or
// declares a size that cannot be right.
bool readBoxHeader(const QByteArray &data, int pos, qint64 fileOffset, qint64 fileSize, Box *out) {
    if (!inRange(data, pos, kBoxHeaderSize))
        return false;

    qint64 size = beUint32(data, pos);
    const QByteArray type = data.mid(pos + 4, 4);
    qint64 headerLen = kBoxHeaderSize;

    if (size == 1) {
        // Size 1 means the real, 64-bit size follows the type.
        if (!inRange(data, pos, 16))
            return false;
        size = static_cast<qint64>(beUint64(data, pos + 8));
        headerLen = 16;
    } else if (size == 0) {
        // Size 0 means the box runs to the end of the file.
        size = fileSize - fileOffset;
    }

    if (size < headerLen || fileOffset + size > fileSize)
        return false;
    out->type = type;
    out->offset = fileOffset;
    out->size = size;
    return true;
}

// A plausible MP4/MOV starts with ftyp (or, for some older QuickTime files,
// moov/mdat directly). Checking this keeps us from "planning" over a file that
// merely happens to have a .mp4 extension.
bool looksLikeIsoContainer(const QByteArray &type) {
    return type == QByteArrayLiteral("ftyp") || type == QByteArrayLiteral("moov") ||
           type == QByteArrayLiteral("mdat") || type == QByteArrayLiteral("free") ||
           type == QByteArrayLiteral("skip") || type == QByteArrayLiteral("wide") ||
           type == QByteArrayLiteral("styp");
}

Mp4RangePlan::Plan indexFoundAt(qint64 indexOffset, qint64 indexSize, qint64 fileSize) {
    Mp4RangePlan::Plan p;
    const qint64 indexEnd = indexOffset + indexSize;

    // Everything from the start through the index: the boxes before it are
    // small headers, and taking them in one run keeps this to a single read.
    p.ranges.append({0, qMin(indexEnd, fileSize)});

    // Plus some media data for the frame itself. When the index sits at the
    // end, the media is at the front and the first range already covers it.
    if (indexEnd < fileSize) {
        const qint64 frameBytes = qMin(kFrameDataBytes, fileSize - indexEnd);
        if (frameBytes > 0)
            p.ranges.append({indexEnd, frameBytes});
    }
    return p;
}

// --- Reading the sample tables ----------------------------------------------
// Everything below walks structures nested inside moov:
//   trak > mdia > (hdlr, minf > stbl > {stss, stco/co64, stsc, stsz, stts})
// Only enough is parsed to answer one question -- which byte range holds the
// keyframe nearest a given point in time -- so no attempt is made to model the
// format in general.

// Finds a child box of `type` within [from, to) of `data`, returning its
// payload range. Boxes are laid end to end, each headed by size + type.
bool findBox(const QByteArray &data, int from, int to, const char *type, int *payloadStart,
             int *payloadEnd) {
    if (from < 0)
        return false;
    // A parent's declared extent can reach past the bytes actually fetched, so
    // never search beyond the buffer whatever the enclosing box claims.
    to = qMin(to, data.size());
    // 64-bit cursor: a box may declare a size larger than an int holds, and
    // truncating that would step the cursor backwards instead of forwards.
    qint64 pos = from;
    while (pos + kBoxHeaderSize <= to) {
        const qint64 size = beUint32(data, static_cast<int>(pos));
        if (size < kBoxHeaderSize || pos + size > to)
            return false; // truncated or malformed: stop rather than guess
        if (data.mid(static_cast<int>(pos) + 4, 4) == QByteArray(type, 4)) {
            *payloadStart = static_cast<int>(pos) + kBoxHeaderSize;
            *payloadEnd = static_cast<int>(pos + size);
            return true;
        }
        pos += size;
    }
    return false;
}

// Full-box tables start with a version/flags word then a 32-bit entry count.
bool tableHeader(const QByteArray &data, int payloadStart, int payloadEnd, quint32 *count,
                 int *entriesStart) {
    if (payloadStart + 8 > payloadEnd)
        return false;
    *count = beUint32(data, payloadStart + 4);
    *entriesStart = payloadStart + 8;
    if (*count > kMaxTableEntries)
        return false;
    return true;
}

// The sample tables of the file's first video track.
struct SampleTables {
    QVector<quint32> syncSamples;   // stss: 1-based sample numbers that are keyframes
    QVector<qint64> chunkOffsets;   // stco/co64: byte offset of each chunk
    QVector<quint32> sampleSizes;   // stsz: per-sample size (empty when uniform)
    quint32 uniformSampleSize = 0;  // stsz: non-zero when every sample is this size
    // stsc runs: (firstChunk, samplesPerChunk)
    QVector<QPair<quint32, quint32>> chunkRuns;
    // stts runs: (sampleCount, duration) in media timescale units
    QVector<QPair<quint32, quint32>> timeRuns;
    quint32 timescale = 0;

    bool usable() const {
        return !syncSamples.isEmpty() && !chunkOffsets.isEmpty() && !chunkRuns.isEmpty() &&
               !timeRuns.isEmpty() && timescale > 0;
    }
};

// True when this trak's handler says "video". A file's first trak is often the
// video one but not reliably -- audio-first files exist -- so it is checked.
bool isVideoTrack(const QByteArray &moov, int mdiaStart, int mdiaEnd) {
    int hs = 0, he = 0;
    if (!findBox(moov, mdiaStart, mdiaEnd, "hdlr", &hs, &he))
        return false;
    // hdlr: version/flags, pre_defined, then the 4-character handler type.
    return he - hs >= 12 && moov.mid(hs + 8, 4) == QByteArrayLiteral("vide");
}

bool readTables(const QByteArray &moov, int mdiaStart, int mdiaEnd, SampleTables *out) {
    int mdhdS = 0, mdhdE = 0;
    if (findBox(moov, mdiaStart, mdiaEnd, "mdhd", &mdhdS, &mdhdE) && mdhdE - mdhdS >= 20) {
        // Version 0 puts the timescale at +12, version 1 (64-bit times) at +20.
        const quint8 version = static_cast<quint8>(beUint32(moov, mdhdS) >> 24);
        const int tsOffset = version == 1 ? 20 : 12;
        if (mdhdS + tsOffset + 4 <= mdhdE)
            out->timescale = beUint32(moov, mdhdS + tsOffset);
    }

    int minfS = 0, minfE = 0, stblS = 0, stblE = 0;
    if (!findBox(moov, mdiaStart, mdiaEnd, "minf", &minfS, &minfE))
        return false;
    if (!findBox(moov, minfS, minfE, "stbl", &stblS, &stblE))
        return false;

    int s = 0, e = 0;
    quint32 n = 0;
    int entries = 0;

    if (findBox(moov, stblS, stblE, "stss", &s, &e) && tableHeader(moov, s, e, &n, &entries)) {
        for (quint32 i = 0; i < n && entries + 4 <= e; ++i, entries += 4)
            out->syncSamples.append(beUint32(moov, entries));
    }

    if (findBox(moov, stblS, stblE, "stco", &s, &e) && tableHeader(moov, s, e, &n, &entries)) {
        for (quint32 i = 0; i < n && entries + 4 <= e; ++i, entries += 4)
            out->chunkOffsets.append(beUint32(moov, entries));
    } else if (findBox(moov, stblS, stblE, "co64", &s, &e) &&
               tableHeader(moov, s, e, &n, &entries)) {
        for (quint32 i = 0; i < n && entries + 8 <= e; ++i, entries += 8)
            out->chunkOffsets.append(static_cast<qint64>(beUint64(moov, entries)));
    }

    if (findBox(moov, stblS, stblE, "stsc", &s, &e) && tableHeader(moov, s, e, &n, &entries)) {
        for (quint32 i = 0; i < n && entries + 12 <= e; ++i, entries += 12)
            out->chunkRuns.append({beUint32(moov, entries), beUint32(moov, entries + 4)});
    }

    if (findBox(moov, stblS, stblE, "stsz", &s, &e) && s + 12 <= e) {
        out->uniformSampleSize = beUint32(moov, s + 4);
        const quint32 count = beUint32(moov, s + 8);
        if (out->uniformSampleSize == 0 && count <= kMaxTableEntries) {
            int p = s + 12;
            for (quint32 i = 0; i < count && p + 4 <= e; ++i, p += 4)
                out->sampleSizes.append(beUint32(moov, p));
        }
    }

    if (findBox(moov, stblS, stblE, "stts", &s, &e) && tableHeader(moov, s, e, &n, &entries)) {
        for (quint32 i = 0; i < n && entries + 8 <= e; ++i, entries += 8)
            out->timeRuns.append({beUint32(moov, entries), beUint32(moov, entries + 4)});
    }

    return out->usable();
}

// Sample number (1-based) at `fraction` of the way through, via stts.
quint32 sampleAtFraction(const SampleTables &t, double fraction) {
    qint64 totalUnits = 0;
    for (const auto &run : t.timeRuns)
        totalUnits += static_cast<qint64>(run.first) * run.second;
    if (totalUnits <= 0)
        return 1;

    const qint64 targetUnits = static_cast<qint64>(totalUnits * fraction);
    qint64 elapsed = 0;
    quint32 sample = 1;
    for (const auto &run : t.timeRuns) {
        const qint64 runUnits = static_cast<qint64>(run.first) * run.second;
        if (elapsed + runUnits >= targetUnits && run.second > 0) {
            const qint64 into = (targetUnits - elapsed) / run.second;
            return sample + static_cast<quint32>(qBound<qint64>(0, into, run.first - 1));
        }
        elapsed += runUnits;
        sample += run.first;
    }
    return sample > 1 ? sample - 1 : 1;
}

// Presentation time of `sample` (1-based) in seconds, by summing the stts runs
// that precede it. Returns -1 when the tables cannot answer.
double timeOfSample(const SampleTables &t, quint32 sample) {
    if (t.timescale == 0 || sample == 0)
        return -1.0;
    qint64 units = 0;
    quint32 seen = 0;
    for (const auto &run : t.timeRuns) {
        const quint32 count = run.first;
        if (sample <= seen + count) {
            units += static_cast<qint64>(sample - 1 - seen) * run.second;
            return static_cast<double>(units) / t.timescale;
        }
        units += static_cast<qint64>(count) * run.second;
        seen += count;
    }
    // Past the last run: the tables are short, so the best answer is the end.
    return static_cast<double>(units) / t.timescale;
}

// Byte offset of `sample` (1-based), by walking stsc's chunk runs and summing
// the sizes of the samples that precede it inside its chunk.
qint64 offsetOfSample(const SampleTables &t, quint32 sample) {
    quint32 chunkIndex = 0;      // 0-based
    quint32 sampleInChunk = 0;   // 0-based within that chunk
    qint64 seen = 0;             // samples accounted for so far

    const quint32 chunkCount = static_cast<quint32>(t.chunkOffsets.size());
    for (int r = 0; r < t.chunkRuns.size(); ++r) {
        const quint32 firstChunk = t.chunkRuns[r].first;          // 1-based
        const quint32 perChunk = t.chunkRuns[r].second;
        // Both fields come from the file. A next-run firstChunk of 0 would
        // underflow to 4 billion chunks here, and a samples-per-chunk of the
        // same order turns the skip loop below into a hang -- so neither is
        // taken further than the chunk table actually goes. On a valid file the
        // clamp and the bound never bite.
        const quint32 lastChunk =
            qMin(chunkCount, (r + 1 < t.chunkRuns.size()) ? t.chunkRuns[r + 1].first - 1
                                                          : chunkCount);
        if (perChunk == 0 || perChunk > kMaxTableEntries || lastChunk < firstChunk)
            continue;
        const quint32 chunks = lastChunk - firstChunk + 1;
        const qint64 samplesHere = static_cast<qint64>(chunks) * perChunk;
        if (seen + samplesHere >= sample) {
            const qint64 into = static_cast<qint64>(sample) - seen - 1;
            if (into < 0)
                return -1; // a sync sample numbered 0: the table is not usable
            chunkIndex = firstChunk - 1 + static_cast<quint32>(into / perChunk);
            sampleInChunk = static_cast<quint32>(into % perChunk);
            break;
        }
        seen += samplesHere;
        chunkIndex = lastChunk; // in case the tables run short
    }

    if (chunkIndex >= static_cast<quint32>(t.chunkOffsets.size()))
        return -1;

    qint64 offset = t.chunkOffsets[chunkIndex];
    // Skip the samples ahead of ours inside this chunk.
    const quint32 firstSampleOfChunk = sample - sampleInChunk;
    for (quint32 s = firstSampleOfChunk; s < sample; ++s) {
        if (t.uniformSampleSize > 0) {
            offset += t.uniformSampleSize;
        } else if (s >= 1 && s <= static_cast<quint32>(t.sampleSizes.size())) {
            offset += t.sampleSizes[s - 1];
        }
    }
    return offset;
}

} // namespace

namespace Mp4RangePlan {

Plan plan(const QByteArray &head, qint64 fileSize) {
    Plan result;
    if (head.size() < kBoxHeaderSize || fileSize <= 0)
        return result;

    Box first;
    if (!readBoxHeader(head, 0, 0, fileSize, &first) || !looksLikeIsoContainer(first.type))
        return result;

    // Walk the chain as far as the bytes in hand allow.
    qint64 offset = 0;
    Box box;
    Box last;
    bool haveLast = false;
    while (offset < fileSize) {
        // Compared in 64-bit before narrowing: a file past 4 GB reaches offsets
        // an int cannot hold, and a wrapped one would land back inside the
        // buffer and be parsed as if it were the next box.
        if (offset < 0 || offset >= head.size())
            break; // ran past what we read
        const int pos = static_cast<int>(offset);
        if (!readBoxHeader(head, pos, offset, fileSize, &box))
            break;

        if (box.type == QByteArrayLiteral("moov"))
            return indexFoundAt(box.offset, box.size, fileSize);

        last = box;
        haveLast = true;
        offset += box.size;
    }

    if (!haveLast)
        return result; // not even one complete box: unusable

    // The chain accounted for everything up to `offset`, so whatever comes next
    // begins there -- for a file whose index trails the media, that is the
    // index. Its size is only knowable by looking, hence the probe.
    const qint64 next = last.offset + last.size;
    if (next <= 0 || next >= fileSize)
        return result; // chain ended at EOF without an index

    result.needsProbe = true;
    result.probeOffset = next;
    result.probeLength = qMin(kBoxHeaderProbe, fileSize - next);
    // Carry the head worth keeping, so refine() can prepend it.
    result.ranges.append({0, qMin<qint64>(first.offset + first.size, fileSize)});
    return result;
}

Plan refine(const Plan &pending, const QByteArray &probe, qint64 fileSize) {
    Plan result;
    if (!pending.needsProbe || probe.size() < kBoxHeaderSize)
        return result;

    Box box;
    if (!readBoxHeader(probe, 0, pending.probeOffset, fileSize, &box))
        return result;
    if (box.type != QByteArrayLiteral("moov") || box.size > kMaxIndexBytes)
        return result; // not the index, or an implausible size -- fall back

    // The media sits before the index here, so the frame data comes from the
    // front of the file rather than after the index.
    result.ranges.append({0, qMin(kFrameDataBytes, pending.probeOffset)});
    result.ranges.append({box.offset, box.size});
    return result;
}


Keyframe keyframeAt(const QByteArray &moov, qint64 moovOffset, qint64 fileSize, double fraction) {
    Q_UNUSED(moovOffset);
    const Keyframe none;
    if (moov.size() < kBoxHeaderSize || fileSize <= 0)
        return none;

    // The buffer is whatever range the caller fetched, so moov may sit at its
    // start, be preceded by the boxes before it (ftyp and friends), or arrive
    // already unwrapped. Walk the chain to find it rather than assuming.
    int start = 0;
    int end = moov.size();
    bool located = false;
    // The cursor is 64-bit on purpose. This buffer is usually the *front* of the
    // file, where mdat declares the whole media run -- and ffmpeg writes that
    // size as a plain 32-bit field for anything under 4 GB. Past 2 GB it no
    // longer fits an int, so truncating it walked the cursor off the front of
    // the buffer and read out of bounds on the next box. Films on a share hit
    // this routinely.
    for (qint64 pos = 0; pos + kBoxHeaderSize <= moov.size();) {
        const int at = static_cast<int>(pos);
        const qint64 size = beUint32(moov, at);
        if (size < kBoxHeaderSize)
            break; // malformed or a 64-bit/extends-to-EOF form we can skip past
        if (moov.mid(at + 4, 4) == QByteArrayLiteral("moov")) {
            start = at + kBoxHeaderSize;
            end = static_cast<int>(qMin<qint64>(pos + size, moov.size()));
            located = true;
            break;
        }
        pos += size; // may leave the buffer, which the loop condition catches
    }
    // No box header found at all: assume the caller handed over moov's payload.
    if (!located && moov.mid(4, 4) != QByteArrayLiteral("moov") &&
        moov.mid(4, 4) != QByteArrayLiteral("ftyp"))
        start = 0;

    // Find the first trak whose handler says video, and read its tables.
    SampleTables tables;
    int pos = start;
    bool found = false;
    while (pos + kBoxHeaderSize <= end) {
        const qint64 size = beUint32(moov, pos);
        if (size < kBoxHeaderSize || pos + size > end)
            break;
        if (moov.mid(pos + 4, 4) == QByteArrayLiteral("trak")) {
            int mdiaS = 0, mdiaE = 0;
            if (findBox(moov, pos + kBoxHeaderSize, pos + static_cast<int>(size), "mdia", &mdiaS,
                        &mdiaE) &&
                isVideoTrack(moov, mdiaS, mdiaE) && readTables(moov, mdiaS, mdiaE, &tables)) {
                found = true;
                break;
            }
        }
        pos += static_cast<int>(size);
    }
    if (!found)
        return none;

    // The sample at `fraction`, then the last keyframe at or before it -- a
    // decoder can only start from a keyframe.
    const quint32 target = sampleAtFraction(tables, fraction);
    quint32 keyframe = tables.syncSamples.first();
    quint32 nextKeyframe = 0;
    for (const quint32 s : tables.syncSamples) {
        if (s > target) {
            nextKeyframe = s;
            break;
        }
        keyframe = s;
    }

    // When the keyframe at or before the seek point is the file's very first
    // sample, seeking to it undoes the reason the grab seeks into the file at
    // all: the opening frame is where the title card and the fade-in live.
    // Measured on a 44 MB clip whose first keyframe covers a full five seconds,
    // that frame is pure black -- mean 0.00, stdev 0.000 -- and it decodes that
    // way from the untouched original too, so no amount of fetching helps.
    // Taking the next keyframe instead lands at 5.0 s on real picture
    // (mean 102.43, stdev 26.08) for the same bytes, since the window is sized
    // per keyframe rather than per file position.
    if (nextKeyframe != 0 && keyframe == tables.syncSamples.first())
        keyframe = nextKeyframe;

    const qint64 offset = offsetOfSample(tables, keyframe);
    if (offset < 0 || offset >= fileSize)
        return none;

    Keyframe result;
    result.range = {offset, qMin(kKeyframeWindowBytes, fileSize - offset)};
    result.seconds = timeOfSample(tables, keyframe);
    return result;
}

Range keyframeRange(const QByteArray &moov, qint64 moovOffset, qint64 fileSize, double fraction) {
    return keyframeAt(moov, moovOffset, fileSize, fraction).range;
}

} // namespace Mp4RangePlan
