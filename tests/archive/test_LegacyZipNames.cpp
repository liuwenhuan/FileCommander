#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QVector>

#include "ArchiveProvider.h"

// A zip written before UTF-8 names were a thing stores the name in whatever
// code page the machine that made it was using, and clears general-purpose bit
// 11 to say so. They are still everywhere: this one came from a Chinese game
// patch, with its one .png named in GBK.
//
// Reading such a name as UTF-8 does not merely display it wrongly -- it
// destroys it. Invalid UTF-8 becomes U+FFFD, and no amount of care downstream
// can turn that back into bytes an extractor could match, so the entry could be
// listed but never opened.
//
// The fixture is built here rather than committed as a binary so the bytes that
// matter (bit 11 clear, name in GBK) are visible and cannot drift.
namespace {

// 使用教程.png in GBK -- the exact bytes from the archive that prompted this.
const QByteArray gbkName() {
    static const char raw[] = "\xCA\xB9\xD3\xC3\xBD\xCC\xB3\xCC.png";
    return QByteArray(raw, sizeof(raw) - 1);
}

// Spelled out rather than linked against zlib: the fixture needs exactly one
// checksum, and the test target has no other reason to depend on it.
quint32 crc32Of(const QByteArray &data) {
    quint32 crc = 0xffffffffu;
    for (char byte : data) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

void appendLE16(QByteArray &out, quint16 value) {
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
}

void appendLE32(QByteArray &out, quint32 value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.append(char((value >> shift) & 0xff));
}

// One stored (uncompressed) entry, general-purpose flags = 0 -- i.e. "this name
// is NOT UTF-8", which is the whole point of the fixture.
QByteArray zipWithOneStoredEntry(const QByteArray &name, const QByteArray &payload) {
    const quint32 crc = crc32Of(payload);
    const quint32 size = quint32(payload.size());

    QByteArray local;
    appendLE32(local, 0x04034b50);
    appendLE16(local, 10);   // version needed
    appendLE16(local, 0);    // flags: bit 11 CLEAR
    appendLE16(local, 0);    // method: stored
    appendLE16(local, 0);    // time
    appendLE16(local, 0x21); // date (1980-01-01)
    appendLE32(local, crc);
    appendLE32(local, size);
    appendLE32(local, size);
    appendLE16(local, quint16(name.size()));
    appendLE16(local, 0);
    local.append(name);
    local.append(payload);

    QByteArray central;
    appendLE32(central, 0x02014b50);
    appendLE16(central, 20); // version made by
    appendLE16(central, 10);
    appendLE16(central, 0);
    appendLE16(central, 0);
    appendLE16(central, 0);
    appendLE16(central, 0x21);
    appendLE32(central, crc);
    appendLE32(central, size);
    appendLE32(central, size);
    appendLE16(central, quint16(name.size()));
    appendLE16(central, 0); // extra
    appendLE16(central, 0); // comment
    appendLE16(central, 0); // disk
    appendLE16(central, 0); // internal attrs
    appendLE32(central, 0); // external attrs
    appendLE32(central, 0); // local header offset
    central.append(name);

    QByteArray end;
    appendLE32(end, 0x06054b50);
    appendLE16(end, 0);
    appendLE16(end, 0);
    appendLE16(end, 1);
    appendLE16(end, 1);
    appendLE32(end, quint32(central.size()));
    appendLE32(end, quint32(local.size()));
    appendLE16(end, 0);

    return local + central + end;
}

// A 2x2 PNG, so "did the extracted file survive" is a real decode rather than a
// byte comparison.
QByteArray tinyPng() {
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

} // namespace

TEST(LegacyZipNames, AnEntryNamedInTheLocalCodePageCanStillBeOpened) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString zipPath = QDir(dir.path()).filePath(QStringLiteral("legacy.zip"));
    QFile zip(zipPath);
    ASSERT_TRUE(zip.open(QIODevice::WriteOnly));
    zip.write(zipWithOneStoredEntry(gbkName(), tinyPng()));
    zip.close();

    QString error;
    ArchiveProvider provider(zipPath, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();

    const QVector<FileInfo> entries = provider.list(QStringLiteral("/"), true);
    ASSERT_EQ(entries.size(), 1);
    const FileInfo &entry = entries.first();

    // The name need not come back as the original characters on a machine whose
    // code page cannot express them -- but it must not contain the replacement
    // character, because that is the lossy step that makes the entry
    // unreachable.
    EXPECT_FALSE(entry.name().contains(QChar(0xFFFD)))
        << "listed as \"" << entry.name().toStdString()
        << "\": decoding a non-UTF-8 name as UTF-8 destroyed it";
    EXPECT_EQ(entry.suffix(), QStringLiteral("png"));

    // The point of all of it: the file can actually be opened.
    const QString extracted = provider.materialize(entry.path());
    ASSERT_FALSE(extracted.isEmpty()) << "materialize() found no such entry";
    EXPECT_TRUE(QFile::exists(extracted)) << extracted.toStdString();
    QImage decoded(extracted);
    EXPECT_FALSE(decoded.isNull()) << "extracted file is not a readable image";
    EXPECT_EQ(decoded.size(), QSize(2, 2));
}
