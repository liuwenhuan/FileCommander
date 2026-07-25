#include <gtest/gtest.h>

#include <QByteArray>

#include "Mp4RangePlan.h"

namespace {

// Builds a top-level box header: 32-bit size, 4-char type.
QByteArray box(const QByteArray &type, quint32 size) {
    QByteArray b;
    b.append(char((size >> 24) & 0xFF));
    b.append(char((size >> 16) & 0xFF));
    b.append(char((size >> 8) & 0xFF));
    b.append(char(size & 0xFF));
    b.append(type);
    return b;
}

// A file whose index sits at the front, right after ftyp.
QByteArray indexFirst(quint32 ftypSize, quint32 moovSize) {
    QByteArray head;
    head.append(box(QByteArrayLiteral("ftyp"), ftypSize));
    head.append(QByteArray(ftypSize - 8, '\0'));
    head.append(box(QByteArrayLiteral("moov"), moovSize));
    return head;
}

// A file whose media comes first and index trails it -- ffmpeg's default.
QByteArray indexLast(quint32 ftypSize, quint32 mdatSize) {
    QByteArray head;
    head.append(box(QByteArrayLiteral("ftyp"), ftypSize));
    head.append(QByteArray(ftypSize - 8, '\0'));
    head.append(box(QByteArrayLiteral("mdat"), mdatSize));
    return head;
}

qint64 totalBytes(const QVector<Mp4RangePlan::Range> &ranges) {
    qint64 sum = 0;
    for (const auto &r : ranges)
        sum += r.second;
    return sum;
}

} // namespace

// An index near the front is fully described by the head alone: no second
// round-trip needed, and the plan covers it exactly.
TEST(Mp4RangePlanTest, IndexAtFrontNeedsNoProbe) {
    const qint64 fileSize = 500LL * 1024 * 1024;
    const Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexFirst(32, 400000), fileSize);

    EXPECT_FALSE(p.needsProbe);
    ASSERT_FALSE(p.ranges.isEmpty());
    EXPECT_EQ(p.ranges.first().first, 0);
    // Must reach past the end of moov (32 + 400000).
    EXPECT_GE(p.ranges.first().second, 32 + 400000);
    // And must not be anywhere near the whole file.
    EXPECT_LT(totalBytes(p.ranges), 2LL * 1024 * 1024);
}

// The reported failure: a long film's index outgrew the old fixed 2 MB head, so
// it decoded to nothing. The plan has to size itself from the container.
TEST(Mp4RangePlanTest, LargeIndexIsCoveredInFull) {
    const qint64 fileSize = 3608LL * 1024 * 1024;
    const quint32 moovSize = 9057565; // measured on a real 3.6 GB file
    const Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexFirst(32, moovSize), fileSize);

    ASSERT_FALSE(p.ranges.isEmpty());
    EXPECT_GE(p.ranges.first().second, 32 + moovSize)
        << "the plan would truncate the index, which decodes to nothing";
}

// When the media comes first, the head cannot know the index's size -- but it
// does know where it starts, which is what the probe is for.
TEST(Mp4RangePlanTest, IndexAtEndAsksForAProbeAtTheRightOffset) {
    const qint64 fileSize = 428LL * 1024 * 1024;
    const quint32 mdatSize = 400LL * 1024 * 1024;
    const Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexLast(32, mdatSize), fileSize);

    ASSERT_TRUE(p.needsProbe);
    EXPECT_EQ(p.probeOffset, 32 + mdatSize) << "probe must land on the box after the media";
    EXPECT_GT(p.probeLength, 0);
    EXPECT_LE(p.probeLength, 64);
}

// Given the probe bytes, the plan completes to the index's real extent.
TEST(Mp4RangePlanTest, ProbeCompletesThePlan) {
    const qint64 fileSize = 428LL * 1024 * 1024;
    const quint32 mdatSize = 400LL * 1024 * 1024;
    Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexLast(32, mdatSize), fileSize);
    ASSERT_TRUE(p.needsProbe);

    const quint32 moovSize = 324895; // measured on a real 428 MB file
    const Mp4RangePlan::Plan done =
        Mp4RangePlan::refine(p, box(QByteArrayLiteral("moov"), moovSize), fileSize);

    ASSERT_EQ(done.ranges.size(), 2);
    // Media from the front, index at its true offset.
    EXPECT_EQ(done.ranges.at(0).first, 0);
    EXPECT_EQ(done.ranges.at(1).first, p.probeOffset);
    EXPECT_EQ(done.ranges.at(1).second, moovSize);
    // The whole point: a fraction of a 428 MB file.
    EXPECT_LT(totalBytes(done.ranges), 1024LL * 1024);
}

// A probe that does not land on an index means the guess was wrong; the caller
// must fall back rather than fetch a bogus range.
TEST(Mp4RangePlanTest, ProbeOnSomethingElseYieldsNoPlan) {
    const qint64 fileSize = 100LL * 1024 * 1024;
    Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexLast(32, 90LL * 1024 * 1024), fileSize);
    ASSERT_TRUE(p.needsProbe);

    const Mp4RangePlan::Plan done =
        Mp4RangePlan::refine(p, box(QByteArrayLiteral("free"), 1000), fileSize);
    EXPECT_TRUE(done.ranges.isEmpty());
}

// A wildly oversized index is a corrupt or hostile size field. Honouring it
// would pull far more than a thumbnail justifies, so refuse and fall back.
TEST(Mp4RangePlanTest, ImplausibleIndexSizeIsRefused) {
    const qint64 fileSize = 4000LL * 1024 * 1024;
    Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexLast(32, 1000LL * 1024 * 1024), fileSize);
    ASSERT_TRUE(p.needsProbe);

    const Mp4RangePlan::Plan done = Mp4RangePlan::refine(
        p, box(QByteArrayLiteral("moov"), 500u * 1024 * 1024), fileSize);
    EXPECT_TRUE(done.ranges.isEmpty());
}

// Not an ISO container (MKV, AVI, junk): no plan, so the caller keeps its
// format-agnostic fallback.
TEST(Mp4RangePlanTest, NonIsoContainerYieldsNoPlan) {
    EXPECT_TRUE(Mp4RangePlan::plan(QByteArrayLiteral("\x1A\x45\xDF\xA3matroska"), 1000).ranges
                    .isEmpty());
    EXPECT_TRUE(Mp4RangePlan::plan(QByteArrayLiteral("RIFF____AVI "), 1000).ranges.isEmpty());
    EXPECT_TRUE(Mp4RangePlan::plan(QByteArray(), 1000).ranges.isEmpty());
    EXPECT_TRUE(Mp4RangePlan::plan(QByteArrayLiteral("\x00\x00"), 1000).ranges.isEmpty());
}

// A size field that overruns the file, or claims less than its own header, is
// malformed -- never trust it into a fetch.
TEST(Mp4RangePlanTest, MalformedSizesAreRejected) {
    // ftyp claiming to extend past EOF.
    EXPECT_TRUE(Mp4RangePlan::plan(box(QByteArrayLiteral("ftyp"), 999999), 1000).ranges.isEmpty());
    // A size smaller than the 8-byte header.
    EXPECT_TRUE(Mp4RangePlan::plan(box(QByteArrayLiteral("ftyp"), 3), 1000).ranges.isEmpty());
}

// Every range must sit inside the file: a plan that reaches past EOF would make
// the fetcher ask a backend for bytes that do not exist.
TEST(Mp4RangePlanTest, RangesStayInsideTheFile) {
    const qint64 fileSize = 2LL * 1024 * 1024;
    const Mp4RangePlan::Plan p = Mp4RangePlan::plan(indexFirst(32, 100000), fileSize);
    for (const auto &r : p.ranges) {
        EXPECT_GE(r.first, 0);
        EXPECT_GT(r.second, 0);
        EXPECT_LE(r.first + r.second, fileSize);
    }
}
