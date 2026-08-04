#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "FileHoles.h"

namespace {

// A download that preallocated its full size and then stopped: `presentBytes`
// of content, the rest left as the zeros the filesystem gave it.
QString writePartialFile(const QTemporaryDir &dir, const QString &name, qint64 total,
                         qint64 presentBytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    QByteArray content(1 << 16, Qt::Uninitialized);
    for (int i = 0; i < content.size(); ++i)
        content[i] = static_cast<char>((i * 7 + 3) % 251 + 1); // never zero
    qint64 written = 0;
    while (written < presentBytes) {
        const qint64 chunk = qMin<qint64>(content.size(), presentBytes - written);
        file.write(content.constData(), chunk);
        written += chunk;
    }
    file.resize(total);
    file.close();
    return path;
}

} // namespace

// The file that prompted this: 2.16 GiB long, 1.4% of it real, and the video
// running out at 134 s of a 2h44m film. Everything past the written part reads
// back as zeros, and nothing about the file's size or its header says so.
TEST(FileHolesTest, AMostlyEmptyFileIsRecognisedPastTheWrittenPart) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePartialFile(dir, QStringLiteral("partial.mp4"),
                                          64 * 1024 * 1024, 2 * 1024 * 1024);
    ASSERT_FALSE(path.isEmpty());

    const fc::HoleReport late = fc::inspectForHoles(path, 40 * 1024 * 1024);
    EXPECT_TRUE(late.sampled);
    EXPECT_TRUE(late.looksIncomplete()) << late.zeroSamples << "/" << late.totalSamples;
}

// The offset probe is accurate where it is pointed, even though the verdict
// does not depend on it: asked about the written part it says the bytes are
// there, while the file as a whole is still recognised as unfinished.
TEST(FileHolesTest, TheOffsetProbeAnswersForTheOffsetItWasGiven) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePartialFile(dir, QStringLiteral("partial.mp4"),
                                          64 * 1024 * 1024, 8 * 1024 * 1024);
    ASSERT_FALSE(path.isEmpty());

    const fc::HoleReport early = fc::inspectForHoles(path, 1 * 1024 * 1024);
    EXPECT_TRUE(early.sampled);
    EXPECT_FALSE(early.aroundOffset) << "these bytes were written";
    // ...and the file is still 7/8 missing, which is what the verdict is about.
    EXPECT_TRUE(early.looksIncomplete()) << early.zeroSamples << "/" << early.totalSamples;
}

// A complete file is never accused, wherever it is asked about.
TEST(FileHolesTest, ACompleteFileIsNeverCalledIncomplete) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writePartialFile(dir, QStringLiteral("whole.mp4"),
                                          16 * 1024 * 1024, 16 * 1024 * 1024);
    ASSERT_FALSE(path.isEmpty());
    for (qint64 offset : {qint64(0), qint64(4 << 20), qint64(15 << 20)}) {
        const fc::HoleReport report = fc::inspectForHoles(path, offset);
        EXPECT_FALSE(report.looksIncomplete()) << "at offset " << offset;
    }
}

// Zeros at one point are not enough on their own: a file may legitimately hold
// a run of them. The verdict needs the rest of the file to be missing too.
//
// The gap here is 8 MiB, wider than the window the probe looks across -- a
// smaller run does not even register, which is the intended behaviour, since a
// hole big enough to stop playback is far larger than that.
TEST(FileHolesTest, AZeroRunInsideAWrittenFileIsNotEnough) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gap.bin"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray content(1 << 20, 'x');
    const QByteArray zeros(1 << 20, char(0));
    for (int i = 0; i < 96; ++i)
        file.write(i >= 40 && i < 48 ? zeros : content);
    file.close();

    const fc::HoleReport report = fc::inspectForHoles(path, 44 << 20);
    EXPECT_TRUE(report.sampled);
    EXPECT_TRUE(report.aroundOffset) << "the probe should see the zeros it was pointed at";
    EXPECT_FALSE(report.looksIncomplete()) << report.zeroSamples << "/" << report.totalSamples;
}

// ...and a run narrower than the probe window is not even noticed, so a few
// zero bytes in the middle of a healthy file can never start this.
TEST(FileHolesTest, ASmallZeroRunGoesUnnoticed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("smallgap.bin"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray content(1 << 20, 'x');
    for (int i = 0; i < 32; ++i)
        file.write(i == 16 ? QByteArray(1 << 20, char(0)) : content);
    file.close();

    const fc::HoleReport report = fc::inspectForHoles(path, 16 << 20);
    EXPECT_FALSE(report.aroundOffset);
    EXPECT_FALSE(report.looksIncomplete());
}

TEST(FileHolesTest, AMissingFileIsNotSampled) {
    const fc::HoleReport report = fc::inspectForHoles(QStringLiteral("Z:/nope/none.mp4"), 0);
    EXPECT_FALSE(report.sampled);
    EXPECT_FALSE(report.looksIncomplete());
}

TEST(FileHolesTest, OffsetForTimeStaysInsideTheFile) {
    EXPECT_EQ(fc::approximateOffsetForTime(1000, 0.0, 100.0), 0);
    EXPECT_EQ(fc::approximateOffsetForTime(1000, 50.0, 100.0), 500);
    // Past the end of the clip clamps rather than running off the file.
    EXPECT_EQ(fc::approximateOffsetForTime(1000, 500.0, 100.0), 999);
    // Nothing to work from.
    EXPECT_LT(fc::approximateOffsetForTime(0, 10.0, 100.0), 0);
    EXPECT_LT(fc::approximateOffsetForTime(1000, 10.0, 0.0), 0);
}
