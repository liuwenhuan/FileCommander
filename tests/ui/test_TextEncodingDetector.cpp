#include <gtest/gtest.h>

#include <clocale>

#include <QByteArray>
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

TEST(TextEncodingDetectorTest, FlagsShortOrNearTieLegacyInputAsAmbiguous) {
    const TextEncodingDetector::Result result = detectHex("D6D0");

    EXPECT_FALSE(result.binary);
    EXPECT_TRUE(result.ambiguous);
}

TEST(TextEncodingDetectorTest, HandlesLargeValidTextWithoutClassifyingItAsBinary) {
    const QByteArray text = QString::fromUtf8(u8"large 文本 日本語 한국어\n").toUtf8().repeated(1024);

    expectEncoding(text, "UTF-8");
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

TEST(TextEncodingDetectorTest, RejectsBomlessUnicodeShapedNulBinaryWithControlsOrInvalidScalars) {
    // Stable NUL lanes alone are insufficient: controls, a malformed surrogate,
    // and a noncharacter all remain binary rather than being promoted to Unicode.
    for (const QByteArray &bytes : {QByteArray::fromHex("0100020003000400"),
                                    QByteArray::fromHex("00000001000000020000000300000004"),
                                    QByteArray::fromHex("410000D8420043004400"),
                                    QByteArray::fromHex("48000000D0FD00006900000021000000")}) {
        const TextEncodingDetector::Result result = TextEncodingDetector::detect(bytes);
        EXPECT_TRUE(result.binary) << result.label.toStdString();
        EXPECT_NE(result.label, QStringLiteral("UTF-16LE"));
        EXPECT_NE(result.label, QStringLiteral("UTF-32BE"));
    }
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
    EXPECT_EQ(status->text(), QStringLiteral("Auto: UTF-8"));
    EXPECT_EQ(editor->toPlainText(), QString::fromUtf8(u8"中文"));

    view.showFile(ambiguousPath);
    EXPECT_TRUE(status->text().startsWith(QStringLiteral("Auto: ")));
    EXPECT_TRUE(status->text().endsWith(QStringLiteral(" (ambiguous)")));

    view.showFile(binaryPath);
    EXPECT_EQ(status->text(), QStringLiteral("Auto: Binary (Hex)"));
    EXPECT_TRUE(editor->toPlainText().startsWith(QStringLiteral("00000000")));
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
    const QString rendered = editor->toPlainText();
    EXPECT_FALSE(rendered.contains(QChar::ReplacementCharacter));
    EXPECT_TRUE(rendered.endsWith(QStringLiteral("\n\n[... truncated ...]")));
    EXPECT_FALSE(rendered.contains(QString::fromUtf8(u8"中")));
}
