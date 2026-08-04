#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "ArchiveProvider.h"
#include "ArchiveVolumes.h"

// Only the FIRST volume of a split set can be opened. Everything here is about
// finding it from whichever member the user happened to click, because the
// alternative is not an error -- it is a wrong answer that looks right (a
// middle volume opens as an empty archive, the last opens with a plausible
// list of unextractable fragments).
namespace {

struct Set {
    QTemporaryDir dir;

    QString touch(const QString &name) {
        const QString path = QDir(dir.path()).filePath(name);
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write("x");
        file.close();
        return path;
    }
};

} // namespace

TEST(ArchiveVolumes, TheFirstVolumeOfARarSetMayBeASelfExtractingExe) {
    Set set;
    // The shape of the archive that prompted this: nine volumes whose first is
    // an SFX stub, so the computed "part01.rar" does not exist at all.
    const QString first = set.touch(QStringLiteral("Palworld.part01.exe"));
    for (int n = 2; n <= 9; ++n)
        set.touch(QStringLiteral("Palworld.part0%1.rar").arg(n));

    const QString middle = QDir(set.dir.path()).filePath(QStringLiteral("Palworld.part05.rar"));
    EXPECT_TRUE(fc::isVolumeMember(middle));
    EXPECT_EQ(fc::firstVolumeOf(middle), first)
        << "resolving to .rar only would find nothing and the set stays unopenable";
    EXPECT_EQ(fc::firstVolumeOf(first), first) << "the first volume resolves to itself";
}

TEST(ArchiveVolumes, TheDigitWidthOfTheSetIsKept) {
    Set set;
    const QString first = set.touch(QStringLiteral("big.part001.rar"));
    const QString later = set.touch(QStringLiteral("big.part017.rar"));
    EXPECT_EQ(fc::firstVolumeOf(later), first) << "part1 is a different file from part001";
}

TEST(ArchiveVolumes, OldStyleRarContinuationsResolveToTheRarItself) {
    Set set;
    const QString first = set.touch(QStringLiteral("movie.rar"));
    set.touch(QStringLiteral("movie.r00"));
    const QString continuation = set.touch(QStringLiteral("movie.r03"));

    EXPECT_TRUE(fc::isVolumeMember(continuation));
    EXPECT_EQ(fc::firstVolumeOf(continuation), first);
    // And the .rar is itself a member, because opening it opens the whole set.
    EXPECT_TRUE(fc::isVolumeMember(first));
    EXPECT_EQ(fc::firstVolumeOf(first), first);
}

TEST(ArchiveVolumes, AnR00WithNoRarBesideItIsTheFirstVolume) {
    Set set;
    const QString first = set.touch(QStringLiteral("orphan.r00"));
    set.touch(QStringLiteral("orphan.r01"));
    EXPECT_EQ(fc::firstVolumeOf(first), first)
        << ".r00 is a continuation only when a .rar exists beside it";
}

TEST(ArchiveVolumes, SevenZipSplitsResolveToThe001) {
    Set set;
    const QString first = set.touch(QStringLiteral("data.7z.001"));
    const QString later = set.touch(QStringLiteral("data.7z.004"));
    EXPECT_TRUE(fc::isVolumeMember(later));
    EXPECT_EQ(fc::firstVolumeOf(later), first);
}

TEST(ArchiveVolumes, AnOrdinaryArchiveIsNotAVolumeMember) {
    Set set;
    const QString plain = set.touch(QStringLiteral("holiday.rar"));
    set.touch(QStringLiteral("notes.zip"));
    EXPECT_FALSE(fc::isVolumeMember(plain))
        << "a lone .rar must not be routed through the volume path";
    EXPECT_FALSE(fc::isVolumeMember(QDir(set.dir.path()).filePath(QStringLiteral("notes.zip"))));
    EXPECT_TRUE(fc::firstVolumeOf(plain).isEmpty());
}

// The fallback for a machine with no 7-Zip. A raw byte split concatenates back
// into the original file, so the volumes can be streamed in order through one
// reader -- no external tool involved. Built here from a real archive so the
// test proves the whole path (recognise member -> resolve first volume ->
// enumerate chain -> read), not just the name arithmetic.
TEST(ArchiveVolumes, ARawSplitIsReadableWithoutAnyExternalTool) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // A .tar built by hand: one 512-byte header plus one 512-byte data block,
    // then the two zero blocks that end the archive. libarchive reads it, and
    // nothing else in the test has to shell out to make it.
    QByteArray tar(512, '\0');
    const QByteArray name = "hello.txt";
    tar.replace(0, name.size(), name);
    tar.replace(100, 8, QByteArray("0000644\0", 8));  // mode
    tar.replace(108, 8, QByteArray("0000000\0", 8));  // uid
    tar.replace(116, 8, QByteArray("0000000\0", 8));  // gid
    tar.replace(124, 12, QByteArray("00000000005\0", 12)); // size: 5
    tar.replace(136, 12, QByteArray("00000000000\0", 12)); // mtime
    tar.replace(156, 1, QByteArray("0", 1));               // typeflag: regular
    tar.replace(257, 8, QByteArray("ustar\00000", 8));
    // Header checksum is computed with the checksum field read as spaces.
    tar.replace(148, 8, QByteArray(8, ' '));
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += static_cast<unsigned char>(tar.at(i));
    QByteArray checksum = QByteArray::number(sum, 8).rightJustified(6, '0');
    checksum.append('\0');
    checksum.append(' ');
    tar.replace(148, 8, checksum);

    QByteArray payload(512, '\0');
    payload.replace(0, 5, QByteArray("hello"));
    tar.append(payload);
    tar.append(QByteArray(1024, '\0')); // end-of-archive

    // Split it down the middle, the way 7-Zip's -v does: a plain byte cut.
    const QString base = QDir(dir.path()).filePath(QStringLiteral("bundle.tar"));
    const int half = tar.size() / 2;
    for (int part = 0; part < 2; ++part) {
        QFile volume(QStringLiteral("%1.%2").arg(base).arg(part + 1, 3, 10, QLatin1Char('0')));
        ASSERT_TRUE(volume.open(QIODevice::WriteOnly));
        volume.write(part == 0 ? tar.left(half) : tar.mid(half));
        volume.close();
    }

    const QString second = QStringLiteral("%1.002").arg(base);
    ASSERT_TRUE(fc::isVolumeMember(second));
    const QString first = fc::firstVolumeOf(second);
    EXPECT_EQ(first, QStringLiteral("%1.001").arg(base));
    EXPECT_TRUE(fc::isRawSplit(first));
    EXPECT_EQ(fc::volumeChain(first).size(), 2);

    // Force the fallback: this machine has 7-Zip, and without this the test
    // would quietly measure that instead of the path it is named after.
    qputenv("FILECOMMANDER_NO_EXTERNAL_ARCHIVE_TOOL", "1");
    struct Restore {
        ~Restore() { qunsetenv("FILECOMMANDER_NO_EXTERNAL_ARCHIVE_TOOL"); }
    } restore;

    // Clicking the SECOND volume must open the whole thing.
    QString error;
    ArchiveProvider provider(second, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    const QVector<FileInfo> entries = provider.list(QStringLiteral("/"), true);
    ASSERT_EQ(entries.size(), 1) << "the split was not read back as one archive";
    EXPECT_EQ(entries.first().name(), QStringLiteral("hello.txt"));
    EXPECT_EQ(entries.first().size(), 5);
}
