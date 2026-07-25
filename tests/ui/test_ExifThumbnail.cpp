#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include "ExifThumbnail.h"

// The EXIF preview path exists because a large remote photo cannot be
// thumbnailed any other way cheaply: a truncated JPEG does not fail to decode,
// it decodes to mostly-grey garbage, so a prefix can never stand in for the
// real image. The embedded preview can -- it is a complete JPEG in the header.
namespace {

// Flat colour compresses to almost nothing, which would make a "large photo"
// smaller than its own preview; the busy pattern keeps the encoded size
// realistic so head-vs-whole-file distinctions actually mean something.
QByteArray encodeJpeg(const QSize &size, int tint, int quality = 90) {
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            row[x] = qRgb((x * 7 + y * 3 + tint) % 256, (x * 13 + y * 5) % 256,
                          (x * 3 + y * 11) % 256);
    }
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", quality);
    return out;
}

// Builds a JPEG carrying `preview` in an EXIF APP1 segment, mirroring how a
// camera writes one: SOI, then APP1("Exif\0\0" + preview), then the real image.
QByteArray withExifPreview(const QByteArray &mainJpeg, const QByteArray &preview) {
    QByteArray payload = QByteArrayLiteral("Exif\x00\x00");
    // A real APP1 also holds TIFF/IFD headers before the preview bytes; the
    // extractor locates the embedded stream itself, so a short filler stands in.
    payload.append(QByteArrayLiteral("MM\x00\x2A\x00\x00\x00\x08"));
    payload.append(preview);

    const int segmentLength = payload.size() + 2; // includes the length field
    QByteArray out;
    out.append(QByteArrayLiteral("\xFF\xD8")); // SOI
    out.append(QByteArrayLiteral("\xFF\xE1")); // APP1
    out.append(static_cast<char>((segmentLength >> 8) & 0xFF));
    out.append(static_cast<char>(segmentLength & 0xFF));
    out.append(payload);
    out.append(mainJpeg.mid(2)); // the main image, minus its own SOI
    return out;
}

} // namespace

TEST(ExifThumbnailTest, ExtractsAnEmbeddedPreview) {
    const QByteArray preview = encodeJpeg(QSize(160, 120), 44);
    const QByteArray photo =
        withExifPreview(encodeJpeg(QSize(1600, 1200), 168), preview);

    const QByteArray found = ExifThumbnail::extract(photo);
    ASSERT_FALSE(found.isEmpty());

    // What comes back must be a decodable image of the *preview*, not the photo.
    QImage decoded;
    ASSERT_TRUE(decoded.loadFromData(found, "JPEG"));
    EXPECT_EQ(decoded.size(), QSize(160, 120));
}

TEST(ExifThumbnailTest, FindsThePreviewInJustTheFileHead) {
    const QByteArray preview = encodeJpeg(QSize(160, 120), 134);
    const QByteArray photo =
        withExifPreview(encodeJpeg(QSize(2000, 1500), 100, 95), preview);
    ASSERT_GT(photo.size(), 60000) << "main image should dwarf the preview";

    // This is the whole point: only the first slice of the remote file is
    // fetched, and the preview still comes out whole.
    const int headBytes = 64 * 1024;
    const QByteArray found = ExifThumbnail::extract(photo.left(headBytes));
    ASSERT_FALSE(found.isEmpty()) << "preview not recoverable from the head alone";

    QImage decoded;
    ASSERT_TRUE(decoded.loadFromData(found, "JPEG"));
    EXPECT_EQ(decoded.size(), QSize(160, 120));
}

TEST(ExifThumbnailTest, ReportsNothingForAJpegWithoutOne) {
    const QByteArray plain = encodeJpeg(QSize(800, 600), 42);
    EXPECT_TRUE(ExifThumbnail::extract(plain).isEmpty());
}

TEST(ExifThumbnailTest, ReportsNothingWhenThePreviewItselfIsCutOff) {
    const QByteArray preview = encodeJpeg(QSize(320, 240), 39);
    const QByteArray photo =
        withExifPreview(encodeJpeg(QSize(1600, 1200), 90), preview);

    // Head ends partway through the embedded preview. Returning those bytes
    // would hand the caller a truncated JPEG -- exactly the mostly-grey result
    // this path exists to avoid -- so it must report nothing instead.
    const QByteArray found = ExifThumbnail::extract(photo.left(200));
    EXPECT_TRUE(found.isEmpty());
}

TEST(ExifThumbnailTest, RejectsNonJpegAndTruncatedInput) {
    EXPECT_TRUE(ExifThumbnail::extract(QByteArray()).isEmpty());
    EXPECT_TRUE(ExifThumbnail::extract(QByteArrayLiteral("\x89PNG\r\n\x1A\n")).isEmpty());
    EXPECT_TRUE(ExifThumbnail::extract(QByteArrayLiteral("\xFF\xD8")).isEmpty());
    // Claims a segment far longer than the data present: must not read past it.
    EXPECT_TRUE(ExifThumbnail::extract(QByteArrayLiteral("\xFF\xD8\xFF\xE1\x7F\xFF""Exif")).isEmpty());
}

// A hostile/corrupt APP1 can declare a length of 2 -- "empty payload" -- while
// still carrying the Exif signature. The span of payload bytes then computes as
// negative, and QByteArray::mid() reads to the end of the buffer on a negative
// length, so the extractor would scan the whole rest of the file for a preview
// that is not in this segment. These bytes come off a remote server, so the
// declared length can never be trusted.
TEST(ExifThumbnailTest, EmptyDeclaredApp1DoesNotReadPastTheSegment) {
    const QByteArray preview = encodeJpeg(QSize(160, 120), 44);

    QByteArray evil;
    evil.append(QByteArrayLiteral("\xFF\xD8")); // SOI
    evil.append(QByteArrayLiteral("\xFF\xE1")); // APP1
    evil.append(QByteArrayLiteral("\x00\x02")); // length 2 == no payload at all
    evil.append(QByteArrayLiteral("Exif\x00\x00"));
    evil.append(preview); // sits outside the declared segment
    evil.append(QByteArrayLiteral("\xFF\xDA"));

    EXPECT_TRUE(ExifThumbnail::extract(evil).isEmpty())
        << "a preview was pulled from beyond the segment the header declared";
}

// Guards the assumption the whole design rests on. If Qt ever starts reporting
// an error for a truncated JPEG, the cheaper "fetch a prefix and decode it"
// approach becomes viable and this path could be revisited. Until then, a
// prefix decodes silently into garbage and must never be used as a thumbnail.
TEST(ExifThumbnailTest, TruncatedJpegStillDecodesWithoutError) {
    const QByteArray photo = encodeJpeg(QSize(2000, 1500), 191, 95);
    ASSERT_GT(photo.size(), 100000);

    QImage partial;
    const bool loaded = partial.loadFromData(photo.left(20000), "JPEG");
    // Non-null despite being a fraction of the file: Qt fills the rest in.
    EXPECT_TRUE(loaded && !partial.isNull())
        << "if this now fails, truncated-prefix decoding became detectable";
}
