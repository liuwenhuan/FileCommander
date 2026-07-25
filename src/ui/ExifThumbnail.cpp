#include "ExifThumbnail.h"

namespace {

// JPEG markers. Every segment is 0xFF followed by one of these.
constexpr quint8 kMarkerPrefix = 0xFF;
constexpr quint8 kSoi = 0xD8;  // start of image
constexpr quint8 kApp1 = 0xE1; // where EXIF lives
constexpr quint8 kSos = 0xDA;  // start of scan: entropy-coded data follows, stop here
constexpr quint8 kEoi = 0xD9;  // end of image

// Markers that stand alone, carrying no length field: SOI, EOI, TEM, and the
// eight restart markers.
bool isStandalone(quint8 marker) {
    return marker == kSoi || marker == kEoi || marker == 0x01 ||
           (marker >= 0xD0 && marker <= 0xD7);
}

quint16 beUint16(const QByteArray &data, int offset) {
    return static_cast<quint16>((static_cast<quint8>(data[offset]) << 8) |
                                static_cast<quint8>(data[offset + 1]));
}

// Locates the complete SOI..EOI JPEG embedded in an EXIF APP1 payload. The
// preview is stored as an ordinary JPEG stream inside the segment, so rather
// than walking the TIFF/IFD structure to find JPEGInterchangeFormat, we scan
// for that stream directly -- the same result, without a TIFF parser, and
// robust to the byte-order and IFD-layout variations across camera vendors.
QByteArray embeddedJpeg(const QByteArray &segment) {
    const int start = segment.indexOf(QByteArrayLiteral("\xFF\xD8\xFF"));
    if (start < 0)
        return {};
    const int end = segment.indexOf(QByteArrayLiteral("\xFF\xD9"), start);
    if (end < 0)
        return {}; // truncated: the preview itself is not fully present
    return segment.mid(start, end + 2 - start);
}

} // namespace

QByteArray ExifThumbnail::extract(const QByteArray &head) {
    // Must start with SOI to be a JPEG at all.
    if (head.size() < 4 || static_cast<quint8>(head[0]) != kMarkerPrefix ||
        static_cast<quint8>(head[1]) != kSoi)
        return {};

    int pos = 2;
    while (pos + 4 <= head.size()) {
        if (static_cast<quint8>(head[pos]) != kMarkerPrefix) {
            ++pos; // fill byte or desync; resync on the next 0xFF
            continue;
        }
        const quint8 marker = static_cast<quint8>(head[pos + 1]);
        if (isStandalone(marker)) {
            pos += 2;
            continue;
        }
        // SOS begins the compressed image data; any EXIF segment precedes it,
        // so there is nothing left to find.
        if (marker == kSos)
            return {};

        const int length = beUint16(head, pos + 2);
        if (length < 2)
            return {}; // malformed segment length
        const int payloadStart = pos + 4;
        const int payloadEnd = pos + 2 + length;

        if (marker == kApp1 && payloadStart + 6 <= head.size() &&
            head.mid(payloadStart, 6) == QByteArrayLiteral("Exif\x00\x00")) {
            // Clamp to what was actually read: on a partial head the segment
            // may be cut off, in which case embeddedJpeg finds no EOI and
            // correctly reports nothing rather than returning a broken image.
            const int available = qMin(payloadEnd, head.size());
            // These bytes come off a remote server, so the declared length is
            // not to be trusted: a segment claiming length 2 (an empty payload)
            // while carrying the Exif signature makes this span negative, and
            // QByteArray::mid reads to the end of the buffer on a negative
            // length -- scanning the rest of the file for a preview that is not
            // in this segment. Never hand it a negative span.
            const int span = available - payloadStart - 6;
            if (span <= 0)
                return {};
            return embeddedJpeg(head.mid(payloadStart + 6, span));
        }
        pos = payloadEnd;
    }
    return {};
}
