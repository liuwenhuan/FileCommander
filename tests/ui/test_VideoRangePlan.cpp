#include <gtest/gtest.h>

#include <QByteArray>

#include "VideoRangePlan.h"

namespace {

using VideoRangePlan::Container;

// Head bytes for a container that is recognised by a fixed signature.
QByteArray headWith(const QByteArray &magic) {
    QByteArray h = magic;
    h.append(QByteArray(64 - h.size(), '\0'));
    return h;
}

// An ISO/MP4 head: 4-byte size, then the type tag.
QByteArray isoHead(const QByteArray &type) {
    QByteArray h;
    h.append(QByteArray(4, '\0'));
    h.append(type);
    h.append(QByteArray(56, '\0'));
    return h;
}

// A transport stream: a 0x47 sync byte every `stride` bytes, `base` in.
QByteArray tsHead(int stride, int base) {
    QByteArray h(base + stride * 5, '\0');
    for (int i = 0; base + i * stride < h.size(); ++i)
        h[base + i * stride] = 0x47;
    return h;
}

qint64 totalBytes(const QVector<VideoRangePlan::Range> &ranges) {
    qint64 sum = 0;
    for (const auto &r : ranges)
        sum += r.second;
    return sum;
}

} // namespace

// The whole point of detecting by content: a file's extension is not evidence.
// Each of these signatures must be read out of the bytes.
TEST(VideoRangePlanTest, DetectsContainersByMagic) {
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral("\x1A\x45\xDF\xA3"))),
              Container::Matroska);
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral("RIFF\x00\x00\x00\x00" "AVI "))),
              Container::Avi);
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral("\x30\x26\xB2\x75"))),
              Container::Asf);
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral("FLV\x01"))), Container::Flv);
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral(".RMF"))), Container::RealMedia);
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral("\x00\x00\x01\xBA"))),
              Container::MpegPs);
}

// ISO-family files must be reported as such so the caller routes them to
// Mp4RangePlan rather than planning them here.
TEST(VideoRangePlanTest, DetectsIsoFamilySoItCanBeRoutedAway) {
    for (const char *type : {"ftyp", "moov", "mdat", "free", "skip", "wide", "styp"})
        EXPECT_EQ(VideoRangePlan::detect(isoHead(type)), Container::Iso) << type;
}

// MPEG-TS has no signature, only a sync byte on a fixed stride -- 188 plain,
// or 192 for the M2TS/AVCHD variant that prefixes each packet with a
// timestamp. Both appear on real shares (.mts from camcorders).
TEST(VideoRangePlanTest, DetectsTransportStreamsOnBothStrides) {
    EXPECT_EQ(VideoRangePlan::detect(tsHead(188, 0)), Container::MpegTs);
    EXPECT_EQ(VideoRangePlan::detect(tsHead(192, 4)), Container::MpegTs);
}

// A stray 0x47 is a common byte; only a repeating stride means a transport
// stream. Guessing wrong here would plan a fetch for a file we cannot read.
TEST(VideoRangePlanTest, LoneSyncByteIsNotATransportStream) {
    QByteArray h(1024, '\0');
    h[0] = 0x47; // and nothing at 188, 376, ...
    EXPECT_EQ(VideoRangePlan::detect(h), Container::Unknown);
}

// Too few bytes, or bytes matching nothing, must not be guessed at.
TEST(VideoRangePlanTest, UnrecognisedHeadsYieldUnknown) {
    EXPECT_EQ(VideoRangePlan::detect(QByteArray()), Container::Unknown);
    EXPECT_EQ(VideoRangePlan::detect(QByteArrayLiteral("short")), Container::Unknown);
    EXPECT_EQ(VideoRangePlan::detect(headWith(QByteArrayLiteral("NOTAVIDEO"))),
              Container::Unknown);
}

// A signature that sits within the first few bytes must still be found when
// that is all there is: an ISO box header is 8 bytes, and refusing to judge it
// would send a perfectly planneable MP4 to the blind fallback instead.
TEST(VideoRangePlanTest, ShortButSufficientHeadsAreStillIdentified) {
    QByteArray justABoxHeader(8, '\0');
    justABoxHeader.replace(4, 4, "ftyp");
    EXPECT_EQ(VideoRangePlan::detect(justABoxHeader), Container::Iso);

    // Truncated one byte further, there is nothing to judge.
    EXPECT_EQ(VideoRangePlan::detect(justABoxHeader.left(7)), Container::Unknown);
}

// Nothing is planned for a container we did not recognise, or for ISO --
// in both cases the caller has somewhere better to go.
TEST(VideoRangePlanTest, NoPlanForUnknownOrIso) {
    EXPECT_TRUE(VideoRangePlan::plan(Container::Unknown, 100LL * 1024 * 1024).isEmpty());
    EXPECT_TRUE(VideoRangePlan::plan(Container::Iso, 100LL * 1024 * 1024).isEmpty());
    EXPECT_TRUE(VideoRangePlan::plan(Container::Matroska, 0).isEmpty());
}

// The frame grab seeks ~10% into the file, so a plan that only fetched a
// prefix would leave a hole exactly where ffmpeg reads. This is the property
// that makes the whole approach work.
TEST(VideoRangePlanTest, PlanCoversTheSeekPoint) {
    const qint64 fileSize = 1000LL * 1024 * 1024;
    const QVector<VideoRangePlan::Range> ranges =
        VideoRangePlan::plan(Container::MpegTs, fileSize);
    ASSERT_FALSE(ranges.isEmpty());

    const qint64 seekAt = static_cast<qint64>(fileSize * 0.10);
    bool covered = false;
    for (const auto &r : ranges)
        covered = covered || (seekAt >= r.first && seekAt < r.first + r.second);
    EXPECT_TRUE(covered) << "ffmpeg would seek into a hole and decode nothing";

    // And the front, which carries the headers naming the tracks.
    EXPECT_EQ(ranges.first().first, 0);
}

// Landing on the seek point is not enough: the decoder has to reach the next
// keyframe from there. A 250-frame GOP at 4 Mbit/s leaves ~1.5 MB between
// keyframes, so a window that stops short of that decodes nothing even though
// it covered the seek exactly.
TEST(VideoRangePlanTest, SeekWindowSpansATypicalKeyframeGap) {
    const qint64 fileSize = 1000LL * 1024 * 1024;
    const qint64 seekAt = static_cast<qint64>(fileSize * 0.10);
    for (const Container kind : {Container::MpegTs, Container::Matroska, Container::Avi}) {
        for (const auto &r : VideoRangePlan::plan(kind, fileSize)) {
            if (seekAt >= r.first && seekAt < r.first + r.second) {
                EXPECT_GE(r.first + r.second - seekAt, 1536LL * 1024)
                    << "window ends before the next keyframe could arrive";
            }
        }
    }
}

// Matroska and AVI keep an index at the end (Cues, idx1), so their plans
// reach the tail. The stream formats have no such index, and fetching a tail
// for them would be a wasted round trip.
TEST(VideoRangePlanTest, OnlyIndexedFormatsFetchTheTail) {
    const qint64 fileSize = 1000LL * 1024 * 1024;
    auto reachesTail = [&](Container c) {
        const QVector<VideoRangePlan::Range> ranges = VideoRangePlan::plan(c, fileSize);
        for (const auto &r : ranges) {
            if (r.first + r.second >= fileSize)
                return true;
        }
        return false;
    };
    EXPECT_TRUE(reachesTail(Container::Matroska));
    EXPECT_TRUE(reachesTail(Container::Avi));
    EXPECT_FALSE(reachesTail(Container::MpegTs));
    EXPECT_FALSE(reachesTail(Container::Flv));
    EXPECT_FALSE(reachesTail(Container::MpegPs));
}

// A file small enough that the ranges would cover most of it is cheaper to
// take whole than to fetch in pieces.
TEST(VideoRangePlanTest, SmallFilesAreTakenWhole) {
    const qint64 fileSize = 512 * 1024;
    const QVector<VideoRangePlan::Range> ranges =
        VideoRangePlan::plan(Container::Flv, fileSize);
    ASSERT_EQ(ranges.size(), 1);
    EXPECT_EQ(ranges.first().first, 0);
    EXPECT_EQ(ranges.first().second, fileSize);
}

// The reason for planning at all: a thumbnail must cost a fraction of the
// file, however long the film is.
TEST(VideoRangePlanTest, PlanStaysSmallOnHugeFiles) {
    const qint64 fileSize = 5317819420LL; // a real 5.3 GB MKV from the sample set
    const QVector<VideoRangePlan::Range> ranges =
        VideoRangePlan::plan(Container::Matroska, fileSize);
    EXPECT_LT(totalBytes(ranges), 8LL * 1024 * 1024);
}

// Every range must sit inside the file and none may overlap: an overlap is a
// byte fetched twice over the network, and a range past EOF asks a backend
// for bytes that do not exist.
TEST(VideoRangePlanTest, RangesAreInBoundsAndDisjoint) {
    const Container kinds[] = {Container::Matroska, Container::Avi, Container::Asf,
                               Container::Flv,      Container::RealMedia,
                               Container::MpegTs,   Container::MpegPs};
    for (const qint64 fileSize : {700LL * 1024, 5LL * 1024 * 1024, 300LL * 1024 * 1024,
                                  9266LL * 1024 * 1024}) {
        for (const Container kind : kinds) {
            const QVector<VideoRangePlan::Range> ranges = VideoRangePlan::plan(kind, fileSize);
            qint64 prevEnd = 0;
            for (const auto &r : ranges) {
                EXPECT_GE(r.first, prevEnd) << "ranges overlap or run backwards";
                EXPECT_GT(r.second, 0);
                EXPECT_LE(r.first + r.second, fileSize) << "range runs past EOF";
                prevEnd = r.first + r.second;
            }
        }
    }
}
