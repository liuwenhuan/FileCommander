#include <gtest/gtest.h>

#include "BinarySniff.h"

#include <QByteArray>
#include <QString>

namespace {

const QString kAnyPath = QStringLiteral("/tmp/sample.bin");

QByteArray utf16(const QString &text, bool littleEndian, bool withBom) {
    QByteArray bytes;
    if (withBom)
        bytes.append(littleEndian ? "\xFF\xFE" : "\xFE\xFF", 2);
    for (const QChar character : text) {
        const ushort unit = character.unicode();
        if (littleEndian) {
            bytes.append(static_cast<char>(unit & 0xff));
            bytes.append(static_cast<char>(unit >> 8));
        } else {
            bytes.append(static_cast<char>(unit >> 8));
            bytes.append(static_cast<char>(unit & 0xff));
        }
    }
    return bytes;
}

QByteArray repeated(const QByteArray &unit, int times) {
    QByteArray bytes;
    for (int i = 0; i < times; ++i)
        bytes.append(unit);
    return bytes;
}

} // namespace

TEST(BinarySniffTest, AsciiSourceIsEditedAsText) {
    const QByteArray sample =
        "#!/bin/sh\nset -eu\necho \"hello, world\"\nexit 0\n";
    EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath));
    EXPECT_EQ(fc::sniff(sample, kAnyPath).reason, fc::SniffReason::DecodedAsText);
}

TEST(BinarySniffTest, Utf8ProseIsEditedAsText) {
    const QByteArray sample = QStringLiteral("文件指挥官是一个双面板文件管理器。\n")
                                  .repeated(8)
                                  .toUtf8();
    EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath));
}

TEST(BinarySniffTest, EmptyFileIsEditedAsText) {
    EXPECT_FALSE(fc::shouldEditAsHex(QByteArray(), kAnyPath));
    EXPECT_EQ(fc::sniff(QByteArray(), kAnyPath).reason, fc::SniffReason::EmptyFile);
}

// A UTF-16 file is half NUL bytes by construction. Routing it to the hex
// editor on that evidence alone would be the single most likely way to get
// this wrong, so all four shapes are pinned.
TEST(BinarySniffTest, Utf16WithBomIsEditedAsText) {
    const QString text = QStringLiteral("The quick brown fox jumps over the lazy dog.\n");
    for (const bool littleEndian : {true, false}) {
        const QByteArray sample = utf16(text, littleEndian, true);
        ASSERT_TRUE(sample.contains('\0'));
        EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath))
            << "little endian: " << littleEndian;
    }
}

TEST(BinarySniffTest, Utf16WithoutBomIsEditedAsText) {
    const QString text =
        QStringLiteral("Configuration file. Do not edit while the service is running.\n")
            .repeated(4);
    const QByteArray sample = utf16(text, true, false);
    ASSERT_TRUE(sample.contains('\0'));
    EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath));
}

TEST(BinarySniffTest, Utf16CjkWithBomIsEditedAsText) {
    const QByteArray sample =
        utf16(QStringLiteral("文件指挥官支持归档与网络后端。\n").repeated(4), true, true);
    EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath));
}

TEST(BinarySniffTest, NulBytesAreEditedAsHex) {
    // A real PNG header: magic, then a length field and chunk name whose NULs
    // are structure rather than text.
    QByteArray sample("\x89PNG\r\n\x1a\n", 8);
    sample.append(QByteArray("\x00\x00\x00\x0DIHDR", 8));
    sample.append(QByteArray("\x00\x00\x02\x00\x00\x00\x01\x40\x08\x06\x00\x00\x00", 13));
    EXPECT_TRUE(fc::shouldEditAsHex(sample, kAnyPath));
    EXPECT_EQ(fc::sniff(sample, kAnyPath).reason, fc::SniffReason::DetectorReportedBinary);
}

TEST(BinarySniffTest, RunOfNulBytesIsEditedAsHex) {
    EXPECT_TRUE(fc::shouldEditAsHex(QByteArray(4096, '\0'), kAnyPath));
}

TEST(BinarySniffTest, ControlCharacterFloodIsEditedAsHex) {
    // No NUL anywhere -- the density of C0 control bytes is the only signal.
    const QByteArray sample = repeated(QByteArray("\x01\x02\x03\x04\x05\x06\x07\x0b", 8), 64);
    ASSERT_FALSE(sample.contains('\0'));
    EXPECT_TRUE(fc::shouldEditAsHex(sample, kAnyPath));
}

// A byte-symmetric sample: read as UTF-16LE and as UTF-16BE it yields the same
// multiset of characters, so TextEncodingDetector cannot separate the two and
// says so. With NUL bytes in the file and no BOM to anchor the guess, that coin
// flip decides whether saving re-encodes the file correctly or corrupts it, so
// it is resolved towards the editor that cannot lose data.
TEST(BinarySniffTest, AmbiguousWideGuessOverNulBytesGoesToHex) {
    const QByteArray unit("\x4E\x8B\x8B\x4E", 4);
    QByteArray sample = repeated(unit, 8);
    sample.append(QByteArray("\x00\x41\x41\x00", 4));
    sample.append(repeated(unit, 8));
    ASSERT_TRUE(sample.contains('\0'));

    const fc::SniffResult result = fc::sniff(sample, kAnyPath);
    EXPECT_TRUE(result.hex);
    EXPECT_EQ(result.reason, fc::SniffReason::AmbiguousWithNulBytes);
}

TEST(BinarySniffTest, LatinOneProseIsEditedAsText) {
    const QByteArray sample =
        QByteArray("Le caf\xE9 est pr\xEAt. La cr\xE8me br\xFBl\xE9""e aussi.\n").repeated(6);
    EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath));
}

// Bytes no candidate encoding accepts, but every one of which is printable in
// some single-byte set. TextEncodingDetector answers "Unknown" here -- refusing
// to name an encoding is not the same as proving the bytes are binary, so the
// file still opens as text. The 0xDC run defeats UTF-16 in both byte orders
// (0xDCDC is an unpaired low surrogate at either alignment) and 0xA0 followed
// by a space defeats every CJK legacy candidate.
TEST(BinarySniffTest, UndecodableButPrintableBytesFallBackToText) {
    const QByteArray sample("R\xE9sum\xE9 \xDC\xDC\xDC\xA0 caf\xE9 \xB5 \xF0 note\n");
    const fc::SniffResult result = fc::sniff(sample, kAnyPath);
    EXPECT_FALSE(result.hex);
    EXPECT_EQ(result.reason, fc::SniffReason::SingleByteFallback);
}

TEST(BinarySniffTest, UndecodableBytesWithControlsAreEditedAsHex) {
    const QByteArray sample("R\xE9sum\xE9 \xDC\xDC\xDC\xA0 \x01\x02 caf\xE9 note\n");
    const fc::SniffResult result = fc::sniff(sample, kAnyPath);
    EXPECT_TRUE(result.hex);
    EXPECT_EQ(result.reason, fc::SniffReason::NoEncodingDecodes);
}

// The sample is a prefix of the file, so a multi-byte character sliced in half
// at the sample boundary is an artefact of how much was read -- not evidence
// that the file is binary.
TEST(BinarySniffTest, Utf8CutMidCharacterIsStillText) {
    QByteArray sample = QStringLiteral("文件指挥官").repeated(20).toUtf8();
    sample.chop(1);
    EXPECT_FALSE(fc::shouldEditAsHex(sample, kAnyPath));
}

TEST(BinarySniffTest, VerdictIgnoresTheFileName) {
    const QByteArray text("plain configuration text, nothing binary here\n");
    QByteArray binary("\x7F""ELF\x02\x01\x01", 7);
    binary.append(QByteArray(64, '\0'));

    EXPECT_FALSE(fc::shouldEditAsHex(text, QStringLiteral("/tmp/archive.7z")));
    EXPECT_TRUE(fc::shouldEditAsHex(binary, QStringLiteral("/tmp/readme.txt")));
}

TEST(BinarySniffTest, TextVerdictCarriesAUsableCodecName) {
    const fc::SniffResult result =
        fc::sniff(QStringLiteral("文件指挥官\n").repeated(8).toUtf8(), kAnyPath);
    ASSERT_FALSE(result.hex);
    EXPECT_EQ(result.codecName, QByteArrayLiteral("UTF-8"));
    EXPECT_FALSE(result.encodingLabel.isEmpty());
}

TEST(BinarySniffTest, HexVerdictNamesNoEncoding) {
    const fc::SniffResult result = fc::sniff(QByteArray(1024, '\0'), kAnyPath);
    ASSERT_TRUE(result.hex);
    EXPECT_TRUE(result.codecName.isEmpty());
    EXPECT_TRUE(result.encodingLabel.isEmpty());
}
