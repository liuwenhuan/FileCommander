#include <gtest/gtest.h>

#include <QByteArray>

#include "TextEncodingDetector.h"

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
