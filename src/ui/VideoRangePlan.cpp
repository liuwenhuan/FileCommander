#include "VideoRangePlan.h"

#include <cstring>

namespace {

// Front of the file: enough for the headers that name tracks and codecs.
// Measured across the sample set, every container's setup data fit well
// inside this.
constexpr qint64 kHeadBytes = 256 * 1024;

// Window at the seek point. ThumbnailCache seeks to 10% of the duration, so
// this has to span from there to the next keyframe -- and a keyframe gap is
// what actually sets the size. A long GOP at a high bitrate puts 1.5 MB
// between keyframes (x264's default 250-frame GOP at 4 Mbit/s does exactly
// this), which a 1 MB window cannot bridge. 2 MB covers that case; beyond it
// the remaining failures are files with no keyframe anywhere near the seek,
// which no window size fixes.
constexpr qint64 kSeekWindowBytes = 2 * 1024 * 1024;

// Back of the file, where a trailing index lives when the format has one
// (Matroska Cues, AVI idx1). Formats with no such index skip this and save
// the round trip.
constexpr qint64 kTailBytes = 2 * 1024 * 1024;

// Fraction of the file the frame grab seeks to. Mirrors the 10% of duration
// ThumbnailCache asks ffmpeg for; for these near-constant-bitrate containers
// the two coincide closely enough to aim a fetch.
constexpr double kSeekFraction = 0.10;

bool startsWith(const QByteArray &head, const char *sig, int len, int at = 0) {
    if (head.size() < at + len)
        return false;
    return std::memcmp(head.constData() + at, sig, static_cast<size_t>(len)) == 0;
}

// A trailing index is only worth a separate range for the formats that keep
// one at the end of the file.
bool hasTrailingIndex(VideoRangePlan::Container kind) {
    return kind == VideoRangePlan::Container::Matroska ||
           kind == VideoRangePlan::Container::Avi;
}

} // namespace

namespace VideoRangePlan {

Container detect(const QByteArray &head) {
    // Eight bytes is the shortest thing worth judging (an ISO box header);
    // every check below guards its own length beyond that. Demanding more
    // would hand ISO files to the fallback that Mp4RangePlan could have
    // planned properly.
    if (head.size() < 8)
        return Container::Unknown;

    // ISO/MP4 family: the type tag sits after the 4-byte size.
    static const char *const kIsoTypes[] = {"ftyp", "moov", "mdat", "free",
                                            "skip", "wide", "styp"};
    for (const char *type : kIsoTypes) {
        if (startsWith(head, type, 4, 4))
            return Container::Iso;
    }

    if (startsWith(head, "\x1A\x45\xDF\xA3", 4))
        return Container::Matroska;
    if (startsWith(head, "RIFF", 4) &&
        (startsWith(head, "AVI ", 4, 8) || startsWith(head, "AVIX", 4, 8)))
        return Container::Avi;
    if (startsWith(head, "\x30\x26\xB2\x75", 4)) // ASF header object GUID
        return Container::Asf;
    if (startsWith(head, "FLV", 3))
        return Container::Flv;
    if (startsWith(head, ".RMF", 4))
        return Container::RealMedia;
    if (startsWith(head, "\x00\x00\x01\xBA", 4))
        return Container::MpegPs;

    // MPEG-TS carries no signature, only a 0x47 sync byte every 188 bytes --
    // or every 192 with the 4-byte timestamp prefix M2TS/AVCHD cameras add.
    // Requiring several in a row keeps a stray 0x47 from being mistaken for a
    // transport stream.
    for (const int stride : {188, 192}) {
        const int base = stride == 192 ? 4 : 0;
        if (head.size() < base + stride * 3 + 1)
            continue;
        bool synced = true;
        for (int i = 0; i < 4; ++i) {
            if (static_cast<quint8>(head[base + i * stride]) != 0x47) {
                synced = false;
                break;
            }
        }
        if (synced)
            return Container::MpegTs;
    }

    return Container::Unknown;
}

QVector<Range> plan(Container kind, qint64 fileSize) {
    QVector<Range> ranges;
    if (fileSize <= 0 || kind == Container::Unknown || kind == Container::Iso)
        return ranges;

    // Small enough that a plan would fetch most of it anyway: take the lot and
    // spare the extra requests.
    const qint64 budget = kHeadBytes + kSeekWindowBytes +
                          (hasTrailingIndex(kind) ? kTailBytes : 0);
    if (fileSize <= budget) {
        ranges.append({0, fileSize});
        return ranges;
    }

    const qint64 head = qMin(kHeadBytes, fileSize);
    ranges.append({0, head});

    // The frames the grab actually decodes. Kept clear of the head so the two
    // never overlap into a double fetch.
    const qint64 seekAt = qMax(head, static_cast<qint64>(fileSize * kSeekFraction));
    if (seekAt < fileSize)
        ranges.append({seekAt, qMin(kSeekWindowBytes, fileSize - seekAt)});

    if (hasTrailingIndex(kind)) {
        const qint64 tailAt = fileSize - kTailBytes;
        // Only when it does not run back into what the seek window covered.
        if (tailAt > seekAt + kSeekWindowBytes)
            ranges.append({tailAt, kTailBytes});
    }
    return ranges;
}

} // namespace VideoRangePlan
