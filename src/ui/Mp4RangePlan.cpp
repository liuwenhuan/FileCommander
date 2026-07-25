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

quint32 beUint32(const QByteArray &data, int offset) {
    return (static_cast<quint32>(static_cast<quint8>(data[offset])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(data[offset + 3]));
}

quint64 beUint64(const QByteArray &data, int offset) {
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
    if (pos + kBoxHeaderSize > data.size())
        return false;

    qint64 size = beUint32(data, pos);
    const QByteArray type = data.mid(pos + 4, 4);
    qint64 headerLen = kBoxHeaderSize;

    if (size == 1) {
        // Size 1 means the real, 64-bit size follows the type.
        if (pos + 16 > data.size())
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
        const int pos = static_cast<int>(offset);
        if (pos < 0 || pos >= head.size())
            break; // ran past what we read
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

} // namespace Mp4RangePlan
