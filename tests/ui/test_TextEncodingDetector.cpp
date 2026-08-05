#include <gtest/gtest.h>

#include <clocale>
#include <limits>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTemporaryDir>

#include "QuickView.h"
#include "TextEncodingDetector.h"
#include "config/Settings.h"

namespace {

TextEncodingDetector::Result detectHex(const char *hex) {
    return TextEncodingDetector::detect(QByteArray::fromHex(hex));
}

void expectEncoding(const QByteArray &bytes, const char *label) {
    const TextEncodingDetector::Result result = TextEncodingDetector::detect(bytes);
    EXPECT_EQ(result.label, QString::fromLatin1(label));
    EXPECT_FALSE(result.binary);
}

} // namespace

TEST(TextEncodingDetectorTest, LabelsAsciiWithoutAmbiguity) {
    const TextEncodingDetector::Result result =
        TextEncodingDetector::detect(QByteArray("plain ASCII text\n"));

    EXPECT_EQ(result.label, QStringLiteral("ASCII"));
    EXPECT_FALSE(result.binary);
    EXPECT_FALSE(result.ambiguous);
}

TEST(TextEncodingDetectorTest, PrefersAsciiOverACoincidentalWidePairingOfAsciiBytes) {
    // Eight ASCII bytes pair into four perfectly valid BMP code units, and read
    // as UTF-16BE they are four CJK ideographs -- a high-scoring wide candidate
    // built entirely out of bytes that cannot be anything but ASCII. Short,
    // even-length ASCII (a Makefile fragment, a .desktop entry, a one-line
    // script) is exactly the shape that produces it.
    const TextEncodingDetector::Result result = TextEncodingDetector::detect(QByteArray("one\ntwo\n"));

    EXPECT_EQ(result.label, QStringLiteral("ASCII"));
    EXPECT_EQ(result.codecName, QByteArrayLiteral("UTF-8"));
    EXPECT_FALSE(result.binary);
    EXPECT_FALSE(result.ambiguous);
    EXPECT_EQ(result.completePrefixBytes, 8);
    EXPECT_EQ(TextEncodingDetector::decode(QByteArray("one\ntwo\n"), result),
              QStringLiteral("one\ntwo\n"));

    // The preference is not a blanket "ASCII bytes are always ASCII": a stable
    // byte lane is structural evidence the pairing is real, which is what keeps
    // BOM-less kana (below) detectable. Only unsupported pairings lose.
    for (const QByteArray &bytes : {QByteArray("ab"), QByteArray("test\n"),
                                    QByteArray("[Desktop Entry]\nExec=x\n"),
                                    QByteArray("CFLAGS = -O2\nall: run\n")}) {
        const TextEncodingDetector::Result ascii = TextEncodingDetector::detect(bytes);
        EXPECT_EQ(ascii.label, QStringLiteral("ASCII")) << bytes.constData();
        EXPECT_EQ(ascii.completePrefixBytes, bytes.size()) << bytes.constData();
    }
}

TEST(TextEncodingDetectorTest, PrefersUtf32BomOverUtf16Prefix) {
    expectEncoding(QByteArray::fromHex("FFFE000041000000"), "UTF-32LE");
    expectEncoding(QByteArray::fromHex("0000FEFF00000041"), "UTF-32BE");
}

TEST(TextEncodingDetectorTest, RecognizesAllUnicodeBoms) {
    expectEncoding(QByteArray::fromHex("EFBBBF4869"), "UTF-8");
    expectEncoding(QByteArray::fromHex("FFFE48006900"), "UTF-16LE");
    expectEncoding(QByteArray::fromHex("FEFF00480069"), "UTF-16BE");
}

TEST(TextEncodingDetectorTest, AcceptsOnlyStrictUtf8) {
    expectEncoding(QString::fromUtf8(u8"中文かな한글").toUtf8(), "UTF-8");

    const TextEncodingDetector::Result invalid = detectHex("C328");
    EXPECT_NE(invalid.label, QStringLiteral("UTF-8"));
}

TEST(TextEncodingDetectorTest, DetectsSupportedLegacyEncodingGrammars) {
    expectEncoding(QByteArray::fromHex("D6D0CEC4"), "GB18030"); // 中文
    // These bytes are valid Big5 for "測文", but they decode as Japanese
    // "日や" too. Kana quality outranks the Chinese candidate deterministically.
    expectEncoding(QByteArray::fromHex("B4FAA4E5"), "EUC-JP");
    expectEncoding(QByteArray::fromHex("93FA967B8CEA82A982C8"), "Shift-JIS"); // 日本語かな
    expectEncoding(QByteArray::fromHex("C6FCCBDCB8ECA4ABA4CA"), "EUC-JP");    // 日本語かな
    expectEncoding(QByteArray::fromHex("C7D1B1B9BEEE"), "EUC-KR");    // 한국어
}

TEST(TextEncodingDetectorTest, RejectsInvalidLegacyGrammar) {
    const TextEncodingDetector::Result result = detectHex("8140FF");

    EXPECT_NE(result.label, QStringLiteral("Shift-JIS"));
    EXPECT_NE(result.label, QStringLiteral("Big5"));
    EXPECT_NE(result.label, QStringLiteral("GB18030"));
}

TEST(TextEncodingDetectorTest, MarksControlHeavyDataAsBinary) {
    QByteArray data;
    data.reserve(512);
    for (int i = 0; i < 512; ++i)
        data.append(static_cast<char>(i & 0x1f));

    const TextEncodingDetector::Result result = TextEncodingDetector::detect(data);

    EXPECT_TRUE(result.binary);
}

TEST(TextEncodingDetectorTest, MarksNullFilledDataAsBinary) {
    const QByteArray data(512 * 1024, '\0');

    const TextEncodingDetector::Result result = TextEncodingDetector::detect(data);

    EXPECT_TRUE(result.binary);
    EXPECT_EQ(result.label, QStringLiteral("Binary"));
}

TEST(TextEncodingDetectorTest, FlagsShortOrNearTieLegacyInputAsAmbiguous) {
    const TextEncodingDetector::Result result = detectHex("D6D0");

    EXPECT_FALSE(result.binary);
    EXPECT_TRUE(result.ambiguous);
}

TEST(TextEncodingDetectorTest, HandlesLargeValidTextWithoutClassifyingItAsBinary) {
    const QByteArray text = QString::fromUtf8(u8"large 文本 日本語 한국어\n").toUtf8().repeated(1024);

    expectEncoding(text, "UTF-8");
}

TEST(TextEncodingDetectorTest, ClassifiesLargeBomlessUtf16CjkWithoutScoreOverflow) {
    const int codePointCount = std::numeric_limits<int>::max() / 40 + 1;
    const QByteArray utf16LeCjk = QByteArray::fromHex("2D4E").repeated(codePointCount);

    const TextEncodingDetector::Result result = TextEncodingDetector::detect(utf16LeCjk);
    EXPECT_EQ(result.label, QStringLiteral("UTF-16LE"));
    EXPECT_FALSE(result.binary);
    EXPECT_FALSE(result.incompleteTail);
    EXPECT_EQ(result.completePrefixBytes, utf16LeCjk.size());
}

TEST(TextEncodingDetectorTest, RejectsBomWithInvalidPayload) {
    EXPECT_NE(detectHex("EFBBBFC328").label, QStringLiteral("UTF-8"));
    EXPECT_NE(detectHex("FFFE00D8").label, QStringLiteral("UTF-16LE"));
    const TextEncodingDetector::Result malformedUtf32 =
        detectHex("FFFE000000001100"); // U+110000, outside Unicode's range
    EXPECT_NE(malformedUtf32.label, QStringLiteral("UTF-32LE"));
    EXPECT_NE(malformedUtf32.label, QStringLiteral("UTF-16LE"));
}

TEST(TextEncodingDetectorTest, DecodesBomPayloadWithoutAByteOrderMarkCharacter) {
    const QByteArray utf8 = QByteArray::fromHex("EFBBBF4869");
    const TextEncodingDetector::Result utf8Result = TextEncodingDetector::detect(utf8);
    EXPECT_EQ(TextEncodingDetector::decode(utf8, utf8Result), QStringLiteral("Hi"));

    const QByteArray utf16 = QByteArray::fromHex("FFFE48006900");
    const TextEncodingDetector::Result utf16Result = TextEncodingDetector::detect(utf16);
    EXPECT_EQ(TextEncodingDetector::decode(utf16, utf16Result), QStringLiteral("Hi"));

    const QByteArray utf32 = QByteArray::fromHex("FFFE00004800000069000000");
    const TextEncodingDetector::Result utf32Result = TextEncodingDetector::detect(utf32);
    EXPECT_EQ(TextEncodingDetector::decode(utf32, utf32Result), QStringLiteral("Hi"));
}

TEST(TextEncodingDetectorTest, SafelyCutsAnIncompleteUtf8CharacterAtThePreviewLimit) {
    const QByteArray source = QByteArray("A") + QString::fromUtf8(u8"中").toUtf8() + "tail";
    const TextEncodingDetector::Result result = TextEncodingDetector::detect(source);
    const QByteArray prefix = TextEncodingDetector::safePrefix(source, 2, result);

    EXPECT_EQ(prefix, QByteArray("A"));
    EXPECT_EQ(TextEncodingDetector::decode(prefix, result), QStringLiteral("A"));
}

TEST(TextEncodingDetectorTest, RecognizesBomlessUtf32BeforeUtf16WhenTextQualityIsHigh) {
    expectEncoding(QByteArray::fromHex("4800000069000000210000000A000000"), "UTF-32LE");
    expectEncoding(QByteArray::fromHex("0000004800000069000000210000000A"), "UTF-32BE");
    expectEncoding(QByteArray::fromHex("4800690021000A00"), "UTF-16LE");
    expectEncoding(QByteArray::fromHex("004800690021000A"), "UTF-16BE");
}

TEST(TextEncodingDetectorTest, RecognizesBomlessUnicodeMadeOnlyOfNonLatinText) {
    // A zero byte lane is only a useful ASCII shortcut. Real UTF-16 CJK has no
    // zero lane at all, while BMP UTF-32 needs only its two scalar high lanes.
    expectEncoding(QByteArray::fromHex("2D4E87652D4E8765"), "UTF-16LE"); // 中文中文
    expectEncoding(QByteArray::fromHex("4E2D65874E2D6587"), "UTF-16BE");
    expectEncoding(QByteArray::fromHex("2D4E0000876500002D4E000087650000"), "UTF-32LE");
    expectEncoding(QByteArray::fromHex("00004E2D0000658700004E2D00006587"), "UTF-32BE");

    expectEncoding(QByteArray::fromHex("4230443046304230"), "UTF-16LE"); // あいうあ
    expectEncoding(QByteArray::fromHex("3042304430463042"), "UTF-16BE");
    expectEncoding(QByteArray::fromHex("5CD56DAD5CD56DAD"), "UTF-16LE"); // 한국한국
}

TEST(TextEncodingDetectorTest, RecognizesBomlessUtf16CjkWithPunctuationAndWhitespace) {
    // 中文， 测试\n -- neutral punctuation and whitespace are normal text,
    // not evidence against an otherwise coherent non-Latin UTF-16 stream.
    expectEncoding(QByteArray::fromHex("2D4E87650CFF20004B6DD58B0A00"), "UTF-16LE");
    expectEncoding(QByteArray::fromHex("4E2D6587FF0C00206D4B8BD5000A"), "UTF-16BE");
}

TEST(TextEncodingDetectorTest, RejectsBomlessUnicodeShapedNulBinaryWithControlsOrInvalidScalars) {
    // Stable NUL lanes alone are insufficient: controls, a malformed surrogate,
    // and a noncharacter all remain binary rather than being promoted to Unicode.
    for (const QByteArray &bytes : {QByteArray::fromHex("0100020003000400"),
                                    QByteArray::fromHex("00000001000000020000000300000004"),
                                    QByteArray::fromHex("48000000D0FD00006900000021000000")}) {
        const TextEncodingDetector::Result result = TextEncodingDetector::detect(bytes);
        EXPECT_TRUE(result.binary) << result.label.toStdString();
        EXPECT_NE(result.label, QStringLiteral("UTF-16LE"));
        EXPECT_NE(result.label, QStringLiteral("UTF-32BE"));
    }

    const QByteArray malformedUtf16Le = QByteArray::fromHex("410000D8420043004400");
    const TextEncodingDetector::Result malformedResult =
        TextEncodingDetector::detect(malformedUtf16Le);
    EXPECT_TRUE(malformedResult.binary);
    EXPECT_EQ(malformedResult.label, QStringLiteral("Binary"));
    EXPECT_TRUE(malformedResult.codecName.isEmpty());
    EXPECT_EQ(malformedResult.completePrefixBytes, 0);
    EXPECT_NE(malformedResult.label, QStringLiteral("UTF-16BE"));
}

TEST(TextEncodingDetectorTest, SafePrefixUsesDetectedUnicodeBoundaryWithoutRedetection) {
    const QByteArray utf16 = QByteArray::fromHex("4100420043004400");
    const TextEncodingDetector::Result utf16Result = TextEncodingDetector::detect(utf16);
    EXPECT_EQ(utf16Result.label, QStringLiteral("UTF-16LE"));
    EXPECT_EQ(TextEncodingDetector::safePrefix(utf16 + QByteArray("tail"), 5, utf16Result),
              QByteArray::fromHex("41004200"));

    const QByteArray utf16Be = QByteArray::fromHex("0041004200430044");
    const TextEncodingDetector::Result utf16BeResult = TextEncodingDetector::detect(utf16Be);
    EXPECT_EQ(utf16BeResult.label, QStringLiteral("UTF-16BE"));
    EXPECT_EQ(TextEncodingDetector::safePrefix(utf16Be + QByteArray("tail"), 5, utf16BeResult),
              QByteArray::fromHex("00410042"));

    const QByteArray utf32 = QByteArray::fromHex("41000000420000004300000044000000");
    const TextEncodingDetector::Result utf32Result = TextEncodingDetector::detect(utf32);
    EXPECT_EQ(utf32Result.label, QStringLiteral("UTF-32LE"));
    EXPECT_EQ(TextEncodingDetector::safePrefix(utf32 + QByteArray("tail"), 6, utf32Result),
              QByteArray::fromHex("41000000"));

    const QByteArray utf32Be = QByteArray::fromHex("00000041000000420000004300000044");
    const TextEncodingDetector::Result utf32BeResult = TextEncodingDetector::detect(utf32Be);
    EXPECT_EQ(utf32BeResult.label, QStringLiteral("UTF-32BE"));
    EXPECT_EQ(TextEncodingDetector::safePrefix(utf32Be + QByteArray("tail"), 6, utf32BeResult),
              QByteArray::fromHex("00000041"));
}

TEST(TextEncodingDetectorTest, LimitsLegacyScoringToRepresentative64KiBSample) {
    constexpr int sampleBytes = 64 * 1024;
    EXPECT_EQ(TextEncodingDetector::legacyScoreSampleBytes(), sampleBytes);

    const QByteArray source = QByteArray::fromHex("D6D0").repeated(5 * 1024 * 1024 / 2);
    const TextEncodingDetector::Result result = TextEncodingDetector::detect(source);
    EXPECT_EQ(result.label, QStringLiteral("GB18030"));
    EXPECT_FALSE(result.binary);
}

TEST(TextEncodingDetectorTest, ValidatesLegacyGrammarAcrossWholeInputBeforeScoringSample) {
    constexpr int sampleBytes = 64 * 1024;
    const QByteArray validSample = QByteArray::fromHex("D6D0").repeated(sampleBytes / 2);
    const TextEncodingDetector::Result result =
        TextEncodingDetector::detect(validSample + QByteArray::fromHex("FF"));

    EXPECT_NE(result.label, QStringLiteral("GB18030"));
    EXPECT_TRUE(result.ambiguous || result.label == QStringLiteral("Unknown"));
}

TEST(TextEncodingDetectorTest, ToleratesOnlyAnIncompleteLegacySequenceAtATruncatedProbeTail) {
    constexpr int previewBytes = 64;
    const QByteArray completeFile = QByteArray::fromHex("D6D0").repeated((previewBytes - 2) / 2) +
                                    QByteArray::fromHex("81308130") + QByteArray("A") +
                                    QByteArray::fromHex("81308130");
    const QByteArray truncatedProbe = completeFile.left(previewBytes + 4);
    ASSERT_EQ(truncatedProbe.right(1), QByteArray::fromHex("81"));

    const TextEncodingDetector::Result strictResult = TextEncodingDetector::detect(truncatedProbe);
    EXPECT_NE(strictResult.label, QStringLiteral("GB18030"));

    const TextEncodingDetector::Result probeResult =
        TextEncodingDetector::detect(truncatedProbe,
                                     TextEncodingDetector::InputEnd::MayBeTruncated);
    ASSERT_EQ(probeResult.label, QStringLiteral("GB18030"));
    const QByteArray preview =
        TextEncodingDetector::safePrefix(truncatedProbe, previewBytes, probeResult);
    EXPECT_EQ(preview.size(), previewBytes - 2);
    EXPECT_FALSE(TextEncodingDetector::decode(preview, probeResult)
                     .contains(QChar::ReplacementCharacter));

    const QByteArray invalidMiddle = QByteArray::fromHex("D6D0FF") + truncatedProbe;
    EXPECT_NE(TextEncodingDetector::detect(invalidMiddle,
                                           TextEncodingDetector::InputEnd::MayBeTruncated)
                  .label,
              QStringLiteral("GB18030"));
}

TEST(TextEncodingDetectorTest, ReportsCompletePrefixForCompleteTextAndBoms) {
    for (const QByteArray &bytes : {
             QByteArray("plain ASCII"),
             QString::fromUtf8(u8"中文").toUtf8(),
             QByteArray::fromHex("EFBBBF4869"),
             QByteArray::fromHex("FFFE48006900"),
             QByteArray::fromHex("FFFE000048000000"),
             QByteArray::fromHex("D6D0CEC4")}) {
        const TextEncodingDetector::Result result = TextEncodingDetector::detect(bytes);
        EXPECT_FALSE(result.binary);
        EXPECT_EQ(result.completePrefixBytes, bytes.size()) << result.label.toStdString();
        EXPECT_FALSE(result.incompleteTail) << result.label.toStdString();
    }
}

TEST(TextEncodingDetectorTest, AcceptsTruncatedUnicodePayloadAfterValidatedBom) {
    const struct {
        QByteArray bom;
        QByteArray completePayload;
        QByteArray truncatedTail;
        const char *label;
    } cases[] = {
        {QByteArray::fromHex("EFBBBF"), QString::fromUtf8(u8"文本").toUtf8(),
         QByteArray::fromHex("F0"), "UTF-8"},
        {QByteArray::fromHex("FFFE"), QByteArray::fromHex("2D4E8765"),
         QByteArray::fromHex("4B"), "UTF-16LE"},
        {QByteArray::fromHex("FEFF"), QByteArray::fromHex("4E2D6587"),
         QByteArray::fromHex("6D"), "UTF-16BE"},
        {QByteArray::fromHex("FFFE0000"), QByteArray::fromHex("2D4E000087650000"),
         QByteArray::fromHex("4B6D"), "UTF-32LE"},
        {QByteArray::fromHex("0000FEFF"), QByteArray::fromHex("00004E2D00006587"),
         QByteArray::fromHex("00006D"), "UTF-32BE"},
    };

    for (const auto &test : cases) {
        const QByteArray probe = test.bom + test.completePayload + test.truncatedTail;
        const TextEncodingDetector::Result strict = TextEncodingDetector::detect(probe);
        EXPECT_NE(strict.label, QString::fromLatin1(test.label));

        const TextEncodingDetector::Result result = TextEncodingDetector::detect(
            probe, TextEncodingDetector::InputEnd::MayBeTruncated);
        ASSERT_EQ(result.label, QString::fromLatin1(test.label));
        EXPECT_EQ(result.bomBytes, test.bom.size());
        EXPECT_TRUE(result.incompleteTail);
        EXPECT_EQ(result.completePrefixBytes, test.bom.size() + test.completePayload.size());
        EXPECT_EQ(TextEncodingDetector::safePrefix(probe, probe.size(), result),
                  test.bom + test.completePayload);
    }
}

TEST(TextEncodingDetectorTest, TruncatedInputReportsAndRemovesIncompleteUtfTails) {
    const struct {
        QByteArray complete;
        QByteArray truncated;
        const char *label;
    } cases[] = {
        {QString::fromUtf8(u8"文本测试").toUtf8(), QByteArray::fromHex("E696"), "UTF-8"},
        {QByteArray::fromHex("4100420043004400"), QByteArray::fromHex("2D"), "UTF-16LE"},
        {QByteArray::fromHex("0041004200430044"), QByteArray::fromHex("4E"), "UTF-16BE"},
        {QByteArray::fromHex("41000000420000004300000044000000"),
         QByteArray::fromHex("2D4E00"), "UTF-32LE"},
        {QByteArray::fromHex("00000041000000420000004300000044"),
         QByteArray::fromHex("00004E"), "UTF-32BE"},
    };

    for (const auto &test : cases) {
        const QByteArray probe = test.complete + test.truncated;
        const TextEncodingDetector::Result strict = TextEncodingDetector::detect(probe);
        EXPECT_NE(strict.label, QString::fromLatin1(test.label));

        const TextEncodingDetector::Result result = TextEncodingDetector::detect(
            probe, TextEncodingDetector::InputEnd::MayBeTruncated);
        ASSERT_EQ(result.label, QString::fromLatin1(test.label));
        EXPECT_TRUE(result.incompleteTail);
        EXPECT_EQ(result.completePrefixBytes, test.complete.size());
        EXPECT_EQ(TextEncodingDetector::safePrefix(probe, probe.size(), result), test.complete);
    }
}

TEST(TextEncodingDetectorTest, ReportsSafeLegacyProbeTailEvenBelowPreviewLimit) {
    const QByteArray probe = QByteArray::fromHex("D6D0CEC481");
    const TextEncodingDetector::Result result = TextEncodingDetector::detect(
        probe, TextEncodingDetector::InputEnd::MayBeTruncated);

    ASSERT_EQ(result.label, QStringLiteral("GB18030"));
    ASSERT_TRUE(result.incompleteTail);
    EXPECT_EQ(result.completePrefixBytes, 4);
    EXPECT_EQ(TextEncodingDetector::safePrefix(probe, 64, result), probe.left(4));
}

TEST(TextEncodingDetectorTest, SafelyTruncatesLargeUtf8AndGb18030AtEncodingBoundaries) {
    constexpr int previewBytes = 5 * 1024 * 1024;
    const QByteArray utf8 = QByteArray(previewBytes - 1, 'A') +
                           QString::fromUtf8(u8"中").toUtf8() + QByteArray("tail");
    const TextEncodingDetector::Result utf8Result = TextEncodingDetector::detect(utf8);
    const QByteArray utf8Prefix = TextEncodingDetector::safePrefix(utf8, previewBytes, utf8Result);
    EXPECT_EQ(utf8Prefix.size(), previewBytes - 1);
    EXPECT_FALSE(TextEncodingDetector::decode(utf8Prefix, utf8Result)
                     .contains(QChar::ReplacementCharacter));

    const QByteArray gb18030 = QByteArray(previewBytes - 1, 'A') +
                               QByteArray::fromHex("D6D0") + QByteArray("tail");
    const TextEncodingDetector::Result gb18030Result = TextEncodingDetector::detect(gb18030);
    ASSERT_EQ(gb18030Result.label, QStringLiteral("GB18030"));
    const QByteArray gb18030Prefix =
        TextEncodingDetector::safePrefix(gb18030, previewBytes, gb18030Result);
    EXPECT_EQ(gb18030Prefix.size(), previewBytes - 1);
    EXPECT_FALSE(TextEncodingDetector::decode(gb18030Prefix, gb18030Result)
                     .contains(QChar::ReplacementCharacter));
}

TEST(TextEncodingDetectorTest, QuickViewShowsAutoDetectionAndBinaryHexStatus) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString textPath = dir.filePath(QStringLiteral("utf8.txt"));
    const QString ambiguousPath = dir.filePath(QStringLiteral("ambiguous.txt"));
    const QString binaryPath = dir.filePath(QStringLiteral("data.bin"));
    {
        QFile text(textPath);
        ASSERT_TRUE(text.open(QIODevice::WriteOnly));
        ASSERT_EQ(text.write(QString::fromUtf8(u8"中文").toUtf8()), 6);
    }
    {
        QFile ambiguous(ambiguousPath);
        ASSERT_TRUE(ambiguous.open(QIODevice::WriteOnly));
        ASSERT_EQ(ambiguous.write(QByteArray::fromHex("D6D0")), 2);
    }
    {
        QFile binary(binaryPath);
        ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
        ASSERT_EQ(binary.write(QByteArray::fromHex("00010203")), 4);
    }

    Settings settings;
    ASSERT_NE(std::setlocale(LC_NUMERIC, "C"), nullptr);
    QuickView view(settings);
    auto *status = view.findChild<QLabel *>(QStringLiteral("textEncodingStatus"));
    auto *editor = view.findChild<QPlainTextEdit *>();
    ASSERT_NE(status, nullptr);
    ASSERT_NE(editor, nullptr);

    view.showFile(textPath);
    ASSERT_TRUE(view.waitForTextIdleForTest());
    EXPECT_EQ(status->text(), QStringLiteral("Auto: UTF-8"));
    EXPECT_EQ(editor->toPlainText(), QString::fromUtf8(u8"中文"));

    view.showFile(ambiguousPath);
    ASSERT_TRUE(view.waitForTextIdleForTest());
    EXPECT_TRUE(status->text().startsWith(QStringLiteral("Auto: ")));
    EXPECT_TRUE(status->text().endsWith(QStringLiteral(" (ambiguous)")));

    view.showFile(binaryPath);
    ASSERT_TRUE(view.waitForTextIdleForTest());
    EXPECT_EQ(status->text(), QStringLiteral("Auto: Binary (Hex)"));
    EXPECT_TRUE(editor->toPlainText().startsWith(QStringLiteral("00000000")));
}

TEST(TextEncodingDetectorTest, QuickViewBoundsHexOutputBeforeTheTextPreviewLimit) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile binary(dir.filePath(QStringLiteral("bounded-binary.bin")));
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    const QByteArray source(1024 * 1024, '\0');
    ASSERT_EQ(binary.write(source), source.size());
    binary.close();

    Settings settings;
    ASSERT_NE(std::setlocale(LC_NUMERIC, "C"), nullptr);
    QuickView view(settings);
    auto *status = view.findChild<QLabel *>(QStringLiteral("textEncodingStatus"));
    auto *editor = view.findChild<QPlainTextEdit *>();
    ASSERT_NE(status, nullptr);
    ASSERT_NE(editor, nullptr);

    view.showFile(binary.fileName());
    ASSERT_TRUE(view.waitForTextIdleForTest());
    EXPECT_EQ(status->text(), QStringLiteral("Auto: Binary (Hex)"));
    const QString rendered = editor->toPlainText();
    EXPECT_TRUE(rendered.startsWith(QStringLiteral("00000000")));
    EXPECT_TRUE(rendered.endsWith(QStringLiteral("\n\n[... truncated ...]")));
    EXPECT_LT(rendered.size(), 2 * 1024 * 1024);
}

TEST(TextEncodingDetectorTest, QuickViewShowsRegistryTransactionLogsAsBoundedHex) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile binary(dir.filePath(
        QStringLiteral("NTUSER.DAT{f250fd3e-49f9-11f1-acef-b36cc46468ba}"
                       ".TMContainer00000000000000000002.regtrans-ms")));
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    const QByteArray source(512 * 1024, '\0');
    ASSERT_EQ(binary.write(source), source.size());
    binary.close();

    Settings settings;
    ASSERT_NE(std::setlocale(LC_NUMERIC, "C"), nullptr);
    QuickView view(settings);
    auto *status = view.findChild<QLabel *>(QStringLiteral("textEncodingStatus"));
    auto *editor = view.findChild<QPlainTextEdit *>();
    ASSERT_NE(status, nullptr);
    ASSERT_NE(editor, nullptr);

    QElapsedTimer elapsed;
    elapsed.start();
    view.showFile(binary.fileName());
    ASSERT_TRUE(view.waitForTextIdleForTest());

    EXPECT_LT(elapsed.elapsed(), 1000);
    EXPECT_EQ(status->text(), QStringLiteral("Auto: Binary (Hex)"));
    const QString rendered = editor->toPlainText();
    EXPECT_TRUE(rendered.startsWith(QStringLiteral("00000000")));
    EXPECT_LT(rendered.size(), 2 * 1024 * 1024);
}

TEST(TextEncodingDetectorTest, QuickViewSafelyTruncatesLargeGb18030ProbeTail) {
    constexpr int previewBytes = 5 * 1024 * 1024;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile text(dir.filePath(QStringLiteral("gb18030-boundary.txt")));
    ASSERT_TRUE(text.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArray::fromHex("D6D0").repeated(previewBytes / 2) +
                              QByteArray::fromHex("81308130") + QByteArray("tail");
    ASSERT_EQ(text.write(source), source.size());
    text.close();

    Settings settings;
    ASSERT_NE(std::setlocale(LC_NUMERIC, "C"), nullptr);
    QuickView view(settings);
    auto *status = view.findChild<QLabel *>(QStringLiteral("textEncodingStatus"));
    auto *editor = view.findChild<QPlainTextEdit *>();
    ASSERT_NE(status, nullptr);
    ASSERT_NE(editor, nullptr);

    view.showFile(text.fileName());
    ASSERT_TRUE(view.waitForTextIdleForTest());
    EXPECT_TRUE(status->text().startsWith(QStringLiteral("Auto: GB18030")));
    const QString rendered = editor->toPlainText();
    EXPECT_FALSE(rendered.contains(QChar::ReplacementCharacter));
    EXPECT_TRUE(rendered.endsWith(QStringLiteral("\n\n[... truncated ...]")));
    EXPECT_EQ(rendered.count(QChar(0x4e2d)), previewBytes / 2);
}

TEST(TextEncodingDetectorTest, QuickViewSafelyTruncatesAtUtf8CharacterBoundary) {
    constexpr int previewBytes = 5 * 1024 * 1024;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile text(dir.filePath(QStringLiteral("boundary.txt")));
    ASSERT_TRUE(text.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArray(previewBytes - 1, 'A') +
                              QString::fromUtf8(u8"中").toUtf8() + QByteArray("tail");
    ASSERT_EQ(text.write(source), source.size());
    text.close();

    Settings settings;
    ASSERT_NE(std::setlocale(LC_NUMERIC, "C"), nullptr);
    QuickView view(settings);
    auto *editor = view.findChild<QPlainTextEdit *>();
    ASSERT_NE(editor, nullptr);

    view.showFile(text.fileName());
    ASSERT_TRUE(view.waitForTextIdleForTest());
    const QString rendered = editor->toPlainText();
    EXPECT_FALSE(rendered.contains(QChar::ReplacementCharacter));
    EXPECT_TRUE(rendered.endsWith(QStringLiteral("\n\n[... truncated ...]")));
    EXPECT_FALSE(rendered.contains(QString::fromUtf8(u8"中")));
}
