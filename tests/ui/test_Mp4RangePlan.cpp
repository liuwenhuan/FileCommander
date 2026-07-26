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

namespace {

// Builds a minimal but structurally valid moov holding one video track whose
// tables describe `samples` frames, a keyframe every `gop`, each `sampleBytes`
// long, one sample per chunk starting at `mediaStart`.
QByteArray videoMoov(int samples, int gop, quint32 sampleBytes, qint64 mediaStart) {
    auto be32 = [](quint32 v) {
        QByteArray b;
        b.append(char((v >> 24) & 0xFF));
        b.append(char((v >> 16) & 0xFF));
        b.append(char((v >> 8) & 0xFF));
        b.append(char(v & 0xFF));
        return b;
    };
    auto full = [&](const QByteArray &type, const QByteArray &body) {
        QByteArray b = be32(quint32(body.size() + 12)) + type + be32(0) + body;
        return b;
    };
    auto plain = [&](const QByteArray &type, const QByteArray &body) {
        return be32(quint32(body.size() + 8)) + type + body;
    };

    QByteArray stss;
    int keys = 0;
    for (int s = 1; s <= samples; s += gop) {
        stss += be32(quint32(s));
        ++keys;
    }
    stss = full(QByteArrayLiteral("stss"), be32(quint32(keys)) + stss);

    QByteArray stco;
    for (int c = 0; c < samples; ++c)
        stco += be32(quint32(mediaStart + qint64(c) * sampleBytes));
    stco = full(QByteArrayLiteral("stco"), be32(quint32(samples)) + stco);

    // One sample per chunk, uniform size, uniform duration (1 unit @ 1 Hz).
    const QByteArray stsc =
        full(QByteArrayLiteral("stsc"), be32(1) + be32(1) + be32(1) + be32(1));
    const QByteArray stsz =
        full(QByteArrayLiteral("stsz"), be32(sampleBytes) + be32(quint32(samples)));
    const QByteArray stts =
        full(QByteArrayLiteral("stts"), be32(1) + be32(quint32(samples)) + be32(1));

    const QByteArray stbl = plain(QByteArrayLiteral("stbl"), stss + stco + stsc + stsz + stts);
    const QByteArray minf = plain(QByteArrayLiteral("minf"), stbl);
    const QByteArray hdlr =
        full(QByteArrayLiteral("hdlr"), be32(0) + QByteArrayLiteral("vide") + be32(0));
    // mdhd v0: creation, modification, timescale, duration.
    const QByteArray mdhd =
        full(QByteArrayLiteral("mdhd"), be32(0) + be32(0) + be32(1) + be32(quint32(samples)));
    const QByteArray mdia = plain(QByteArrayLiteral("mdia"), mdhd + hdlr + minf);
    const QByteArray trak = plain(QByteArrayLiteral("trak"), mdia);
    return plain(QByteArrayLiteral("moov"), trak);
}

} // namespace

// The frame grab seeks to 10% of the duration, so the plan has to reach the
// keyframe there -- not the start of the media. This is what a fixed window
// cannot do reliably, since the keyframe spacing is set by the encoder.
//
// Here the opening keyframe spans the whole first tenth, so "the keyframe at or
// before the seek point" is sample #1 -- the file's very first frame, which is
// where title cards and fade-ins live. The next one is taken instead; see
// KeyframeAtSkipsAFirstFrameThatWouldBeTheOpeningShot for why that matters.
TEST(Mp4RangePlanTest, KeyframeRangeLandsOnTheSeekPoint) {
    constexpr int samples = 1000;
    constexpr int gop = 250;              // keyframes at 1, 251, 501, 751
    constexpr quint32 sampleBytes = 4096;
    constexpr qint64 mediaStart = 100000;
    const QByteArray moov = videoMoov(samples, gop, sampleBytes, mediaStart);

    const qint64 fileSize = mediaStart + qint64(samples) * sampleBytes + 1;
    const Mp4RangePlan::Range r = Mp4RangePlan::keyframeRange(moov, 0, fileSize, 0.10);

    // 10% of 1000 samples is sample 100. The keyframe at or before it is #1, so
    // the answer is #251 -- one GOP in, still near the requested point.
    ASSERT_GT(r.second, 0) << "no keyframe located";
    EXPECT_EQ(r.first, mediaStart + 250 * qint64(sampleBytes))
        << "should skip the opening keyframe and take the next one";
}

// A first keyframe that covers the seek point means seeking to it lands on
// frame zero -- the one shot the 10% seek exists to avoid. Measured on a real
// 44 MB clip (a 5-second opening GOP), that frame decodes to pure black from
// the untouched original, so fetching more bytes cannot help; only choosing a
// different keyframe can. The next one is free: the window is sized per
// keyframe, not per file position.
TEST(Mp4RangePlanTest, KeyframeAtSkipsAFirstFrameThatWouldBeTheOpeningShot) {
    constexpr int samples = 1000;
    constexpr int gop = 250;
    constexpr quint32 sampleBytes = 4096;
    constexpr qint64 mediaStart = 100000;
    const QByteArray moov = videoMoov(samples, gop, sampleBytes, mediaStart);
    const qint64 fileSize = mediaStart + qint64(samples) * sampleBytes + 1;

    const Mp4RangePlan::Keyframe kf = Mp4RangePlan::keyframeAt(moov, 0, fileSize, 0.10);
    ASSERT_TRUE(kf.valid());
    EXPECT_EQ(kf.range.first, mediaStart + 250 * qint64(sampleBytes));
    // Timescale is 1 Hz with one unit per sample, so sample #251 plays at 250 s.
    EXPECT_DOUBLE_EQ(kf.seconds, 250.0)
        << "the reported time must be the chosen keyframe's own, not the seek point";
}

// The skip must not fire when the seek point genuinely falls past a later
// keyframe -- that would push the thumbnail away from the requested fraction
// for no reason.
TEST(Mp4RangePlanTest, KeyframeAtKeepsALaterKeyframeAsItIs) {
    const QByteArray moov = videoMoov(1000, 250, 4096, 100000);
    const qint64 fileSize = 100000 + 1000LL * 4096 + 1;

    const Mp4RangePlan::Keyframe kf = Mp4RangePlan::keyframeAt(moov, 0, fileSize, 0.55);
    ASSERT_TRUE(kf.valid());
    // Sample 550 -> keyframe #501, which is not the first, so it stands.
    EXPECT_EQ(kf.range.first, 100000 + 500LL * 4096);
    EXPECT_DOUBLE_EQ(kf.seconds, 500.0);
}

// A file with exactly one keyframe has nothing to skip to. Reporting it is
// still right: it is the only frame a decoder can start from, and the caller
// needs the range regardless.
TEST(Mp4RangePlanTest, KeyframeAtKeepsTheOnlyKeyframeItHas) {
    constexpr qint64 mediaStart = 100000;
    const QByteArray moov = videoMoov(100, 1000, 4096, mediaStart); // one keyframe: #1
    const qint64 fileSize = mediaStart + 100LL * 4096 + 1;

    const Mp4RangePlan::Keyframe kf = Mp4RangePlan::keyframeAt(moov, 0, fileSize, 0.10);
    ASSERT_TRUE(kf.valid());
    EXPECT_EQ(kf.range.first, mediaStart);
    EXPECT_DOUBLE_EQ(kf.seconds, 0.0);
}

// Half way in, the answer must be the keyframe before that point (#501), not
// the first one -- otherwise the excerpt holds the wrong part of the file.
TEST(Mp4RangePlanTest, KeyframeRangeTracksTheRequestedFraction) {
    constexpr int samples = 1000;
    constexpr int gop = 250;
    constexpr quint32 sampleBytes = 4096;
    constexpr qint64 mediaStart = 100000;
    const QByteArray moov = videoMoov(samples, gop, sampleBytes, mediaStart);
    const qint64 fileSize = mediaStart + qint64(samples) * sampleBytes + 1;

    const Mp4RangePlan::Range r = Mp4RangePlan::keyframeRange(moov, 0, fileSize, 0.55);
    ASSERT_GT(r.second, 0);
    // Sample 550 -> keyframe 501 -> offset mediaStart + 500 * sampleBytes.
    EXPECT_EQ(r.first, mediaStart + 500 * qint64(sampleBytes));
}

// The buffer handed in is whatever range was fetched, which for a file with a
// leading index starts at byte 0 -- ftyp first, moov after it. Finding moov
// inside the buffer is what makes that case work.
TEST(Mp4RangePlanTest, KeyframeRangeFindsMoovBehindLeadingBoxes) {
    const QByteArray moov = videoMoov(1000, 250, 4096, 100000);
    QByteArray withFtyp;
    withFtyp.append(QByteArray("\x00\x00\x00\x10", 4));
    withFtyp.append(QByteArrayLiteral("ftyp"));
    withFtyp.append(QByteArray(8, '\0'));
    withFtyp.append(moov);

    const qint64 fileSize = 100000 + 1000LL * 4096 + 1;
    const Mp4RangePlan::Range r = Mp4RangePlan::keyframeRange(withFtyp, 0, fileSize, 0.10);
    EXPECT_GT(r.second, 0) << "moov was not found behind the leading ftyp box";
}

// Audio-only or table-less input must report nothing rather than a bogus
// offset, so the caller keeps its fixed-window fallback.
TEST(Mp4RangePlanTest, KeyframeRangeDeclinesWithoutAVideoTrack) {
    EXPECT_EQ(Mp4RangePlan::keyframeRange(QByteArray(), 0, 1000, 0.10).second, 0);
    EXPECT_EQ(Mp4RangePlan::keyframeRange(QByteArrayLiteral("not a box"), 0, 1000, 0.10).second, 0);
    // A moov with no trak at all.
    QByteArray empty;
    empty.append(QByteArray("\x00\x00\x00\x08", 4));
    empty.append(QByteArrayLiteral("moov"));
    EXPECT_EQ(Mp4RangePlan::keyframeRange(empty, 0, 1000, 0.10).second, 0);
}

// --- Untrusted input --------------------------------------------------------
// Every size below comes from the file's own bytes, and the buffer is only a
// fetched slice of a remote file -- so it can be short, cut mid-box, or not an
// MP4 at all. Nothing here may read outside the buffer; the answer is simply
// "no keyframe", which the caller already treats as "use the fixed window".

// The crash seen on a share full of films, reproduced from a real file's own
// header ("Newcomer GDoL.mp4", 2,574,833,573 bytes: ftyp 32, free 8, then mdat
// declaring 2,572,706,270).
//
// Nothing here is corrupt -- this is what ffmpeg writes for any file under
// 4 GB, since a 32-bit size still fits. Past 2 GB it no longer fits an *int*,
// and the walk advanced the cursor by the truncated value: 40 + (int)2572706270
// = -1722260986, still small enough to pass the "inside the buffer" test, so
// the next box header was read from well before the start of the buffer.
//
// The buffer is the front of the file because this layout puts the index last,
// so the plan's first range is media and keyframeRangeFor() offers it to the
// parser first. Every film of this size on the share hit it.
TEST(Mp4RangePlanTest, KeyframeAtSurvivesAMdatBiggerThanAnInt) {
    const qint64 fileSize = 2574833573LL;
    const quint32 mdatSize = 2572706270u; // > 2^31, and 32-bit on disk

    QByteArray buf;
    buf.append(box(QByteArrayLiteral("ftyp"), 32));
    buf.append(QByteArray(24, '\0'));
    buf.append(box(QByteArrayLiteral("free"), 8));
    buf.append(box(QByteArrayLiteral("mdat"), mdatSize));
    buf.append(QByteArray(256 * 1024, '\xAB')); // media bytes, as actually fetched

    EXPECT_EQ(Mp4RangePlan::keyframeAt(buf, 0, fileSize, 0.10).range.second, 0);

    // plan() sees the same chain, and must still place the probe on the index
    // that follows the media rather than on an offset folded through an int.
    const Mp4RangePlan::Plan p = Mp4RangePlan::plan(buf, fileSize);
    if (p.needsProbe)
        EXPECT_EQ(p.probeOffset, 40 + qint64(mdatSize));
}

// Same shape, one step further: a size whose low bits land the cursor back
// inside the buffer rather than merely below zero. Walking must still stop.
TEST(Mp4RangePlanTest, KeyframeAtDoesNotLoopOnAWrappingBoxSize) {
    QByteArray buf;
    buf.append(box(QByteArrayLiteral("ftyp"), 16));
    buf.append(QByteArray(8, '\0'));
    buf.append(box(QByteArrayLiteral("mdat"), 0x80000000u)); // wraps to INT_MIN
    buf.append(QByteArray(4096, '\x11'));

    EXPECT_EQ(Mp4RangePlan::keyframeAt(buf, 0, 4LL * 1024 * 1024 * 1024, 0.10).range.second, 0);
}

// A box header cut in half by the end of the fetched range, at every length a
// truncation can leave behind.
TEST(Mp4RangePlanTest, KeyframeAtHandlesEveryTruncationOfAValidFile) {
    QByteArray full;
    full.append(box(QByteArrayLiteral("ftyp"), 16));
    full.append(QByteArray(8, '\0'));
    full.append(videoMoov(1000, 250, 4096, 100000));
    const qint64 fileSize = 100000 + 1000LL * 4096 + 1;

    // The whole thing works; every prefix of it must at worst decline.
    ASSERT_TRUE(Mp4RangePlan::keyframeAt(full, 0, fileSize, 0.10).valid());
    for (int n = 0; n < full.size(); ++n)
        Mp4RangePlan::keyframeAt(full.left(n), 0, fileSize, 0.10); // must not read out of bounds
}

// The index itself declares a size, and a range request that came back short
// leaves moov claiming more bytes than arrived. The tables inside are then cut
// off wherever the buffer ends.
TEST(Mp4RangePlanTest, KeyframeAtHandlesAnIndexTruncatedMidTable) {
    const QByteArray moov = videoMoov(1000, 250, 4096, 100000);
    const qint64 fileSize = 100000 + 1000LL * 4096 + 1;
    // Keep moov's declared size but deliver only part of the payload.
    for (int keep : {12, 40, 200, 1000, moov.size() / 2, moov.size() - 4})
        Mp4RangePlan::keyframeAt(moov.left(keep), 0, fileSize, 0.10);
}

// A hostile or corrupt size field on any nested box: each one is rewritten to
// claim it runs far past the end of the buffer.
TEST(Mp4RangePlanTest, KeyframeAtRejectsNestedBoxesClaimingImpossibleSizes) {
    const QByteArray good = videoMoov(200, 50, 4096, 100000);
    const qint64 fileSize = 100000 + 200LL * 4096 + 1;

    for (int pos = 0; pos + 8 <= good.size(); ++pos) {
        QByteArray bad = good;
        bad[pos] = char(0xFF);
        bad[pos + 1] = char(0xFF);
        bad[pos + 2] = char(0xFF);
        bad[pos + 3] = char(0xF0);
        Mp4RangePlan::keyframeAt(bad, 0, fileSize, 0.10);
    }
}

// A table header can claim any entry count; the entries themselves may simply
// not be there. Reading them must stop at the buffer, not at the count.
TEST(Mp4RangePlanTest, KeyframeAtHandlesTablesClaimingMoreEntriesThanExist) {
    QByteArray moov = videoMoov(200, 50, 4096, 100000);
    const qint64 fileSize = 100000 + 200LL * 4096 + 1;

    for (const QByteArray &table : {QByteArrayLiteral("stss"), QByteArrayLiteral("stco"),
                                    QByteArrayLiteral("stsc"), QByteArrayLiteral("stsz"),
                                    QByteArrayLiteral("stts")}) {
        const int at = moov.indexOf(table);
        ASSERT_GT(at, 0) << table.constData();
        QByteArray bad = moov;
        // Overwrite the count word that follows the version/flags word.
        for (int i = 0; i < 4; ++i)
            bad[at + 8 + i] = char(0x7F);
        Mp4RangePlan::keyframeAt(bad, 0, fileSize, 0.10);
    }
}

// Arbitrary bytes: a share holds files whose extension lies, and a failed range
// read can hand back an error page. Whatever the bytes are, the parser walks
// them, so it must never step outside the buffer -- and must finish quickly.
TEST(Mp4RangePlanTest, KeyframeAtWalksArbitraryBytesWithoutReadingOutside) {
    quint32 seed = 0x12345678u;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return char((seed >> 16) & 0xFF);
    };
    for (int trial = 0; trial < 400; ++trial) {
        QByteArray buf(1 + (trial % 517), '\0');
        for (int i = 0; i < buf.size(); ++i)
            buf[i] = next();
        Mp4RangePlan::keyframeAt(buf, 0, 4LL * 1024 * 1024 * 1024, 0.10);
        // Prefixed with a real ftyp, so the walk gets past the first box.
        QByteArray withFtyp = box(QByteArrayLiteral("ftyp"), 16) + QByteArray(8, '\0') + buf;
        Mp4RangePlan::keyframeAt(withFtyp, 0, 4LL * 1024 * 1024 * 1024, 0.10);
    }
}

// plan() reads the same untrusted chain. A file past 4 GB whose boxes overflow
// a 32-bit cursor must not be "planned" from a mis-parsed offset.
TEST(Mp4RangePlanTest, PlanHandlesBoxChainsPastFourGigabytes) {
    const qint64 fileSize = 6LL * 1024 * 1024 * 1024;
    QByteArray head;
    head.append(box(QByteArrayLiteral("ftyp"), 16));
    head.append(QByteArray(8, '\0'));
    // 64-bit mdat: size 1, type, then the real length.
    head.append(box(QByteArrayLiteral("mdat"), 1));
    const qint64 mdatSize = 5LL * 1024 * 1024 * 1024;
    for (int i = 7; i >= 0; --i)
        head.append(char((mdatSize >> (i * 8)) & 0xFF));

    const Mp4RangePlan::Plan p = Mp4RangePlan::plan(head, fileSize);
    for (const auto &r : p.ranges) {
        EXPECT_GE(r.first, 0);
        EXPECT_LE(r.first + r.second, fileSize);
    }
    if (p.needsProbe) {
        EXPECT_GE(p.probeOffset, 0);
        EXPECT_LE(p.probeOffset + p.probeLength, fileSize);
    }
}
