#pragma once

#include <QByteArray>
#include <QPair>
#include <QVector>

// Works out which byte ranges of a video a thumbnail actually needs, for the
// containers that are not ISO/MP4 (those are Mp4RangePlan's job).
//
// The starting point is that a file's extension does not tell you its format.
// On a real 9,500-file share, every single `.mkv` in one directory turned out
// to be MPEG-TS, and one `.asf` was really a RIFF/AVI. So the container is
// identified from the bytes, and the plan follows from that.
//
// The second, less obvious point is what the frame grab actually asks for.
// ThumbnailCache seeks to 10% of the *duration* before taking a frame, which
// for these roughly-constant-bitrate formats is about 10% into the *file*. A
// plain prefix -- however long -- therefore drops ffmpeg into a hole in the
// sparse temp file and decodes to nothing. What works is small and specific:
//
//   * the front, holding the headers that name the tracks and codecs;
//   * a window at the seek point, holding the frames actually decoded;
//   * the back, where formats that carry a trailing index keep it
//     (Matroska's Cues, AVI's idx1).
//
// Measured against the shipped fixed 2 MB head + 2 MB tail over 30 real
// non-ISO files from an SMB share, this decodes more of them (25 vs 21) while
// fetching 1.4x fewer bytes, and regressed none. Notably it also beat
// hand-written EBML and RIFF index parsers, which cost far more code and did
// worse -- an index tells you where the frames are, but a thumbnail still
// needs the frames themselves, and the seek window supplies those directly.
// Hence: no per-format parsers here, just detection.
namespace VideoRangePlan {

// A byte range: (offset, length).
using Range = QPair<qint64, qint64>;

// Containers distinguished by magic bytes. `Iso` means MP4/MOV/3GP/M4V, which
// the caller should route to Mp4RangePlan instead.
enum class Container {
    Unknown,
    Iso,      // ftyp/moov/... -- handled by Mp4RangePlan
    Matroska, // MKV, WebM
    Avi,      // RIFF/AVI
    Asf,      // WMV, ASF
    Flv,
    RealMedia, // RM, RMVB
    MpegTs,    // .ts, .m2ts/.mts, and plenty of mislabelled files
    MpegPs,    // MPEG program stream: .mpg, .vob
};

// Identifies the container from the first bytes of the file. `head` should be
// the same buffer the caller already read for Mp4RangePlan -- this costs no
// extra round trip.
Container detect(const QByteArray &head);

// Ranges to fetch for a non-ISO container. Returns empty when `kind` is one
// this does not plan for (Unknown or Iso), so the caller falls back.
QVector<Range> plan(Container kind, qint64 fileSize);

// The one run of bytes a transport stream needs, to be written contiguously
// rather than into a sparse file. Returns (0, 0) for a file too small to be
// worth seeking into, meaning "just take the whole thing".
//
// MPEG-TS is the one container that a sparse excerpt actively breaks. It has no
// index and no global header: a demuxer syncs by finding 0x47 every 188 bytes,
// so holes read as zeroes, sync never happens, and it rescans byte by byte
// across the file's entire apparent length -- 25.6 s on a 1.5 GB share file
// against 0.2 s for the identical bytes laid out end to end. Dropping the true
// offsets costs nothing here precisely because a stream has no absolute
// pointers to preserve.
//
// The offset is rounded DOWN to a packet boundary so the demuxer's first sync
// attempt lands on a 0x47 rather than mid-packet. Note 188 is not a power of
// two, so this must be a division, not a bit mask.
Range contiguousStreamRange(qint64 fileSize);

} // namespace VideoRangePlan
