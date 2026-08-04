#include "BinarySniff.h"

// Layering note: TextEncodingDetector physically lives in src/ui, one layer
// above this one, so viewer cannot link the `ui` target. Its two translation
// units are compiled into `viewer` directly (see src/viewer/CMakeLists.txt)
// rather than duplicated here -- a second, drifting copy of that grammar is
// exactly the failure this file exists to avoid. The clean fix is to relocate
// TextEncodingDetector down into src/viewer (or src/core); that edit belongs
// to whoever owns src/ui.
#include "text/TextEncodingDetector.h"

#include <QChar>

namespace fc {
namespace {

constexpr int kSampleBytes = 64 * 1024;

bool isPlainControl(uchar value) {
    return value < 0x20 && value != '\t' && value != '\n' && value != '\r';
}

bool containsNul(const QByteArray &data) {
    for (char byte : data) {
        if (byte == '\0')
            return true;
    }
    return false;
}

// Rescue path for the encodings TextEncodingDetector deliberately does not
// name. Its candidate list is Unicode plus the CJK multi-byte legacy sets, so
// an ISO-8859-x or CP125x file -- perfectly ordinary text in Europe -- comes
// back as "Unknown". Refusing to name an encoding is not the same as proving
// the bytes are binary, so before sending such a file to the hex editor we ask
// the weaker question the detector never asks: does every byte look like a
// printable character in *some* single-byte set?
bool looksLikeSingleByteText(const QByteArray &data) {
    qint64 soft = 0;
    for (char byte : data) {
        const uchar value = static_cast<uchar>(byte);
        if (value == 0 || isPlainControl(value) || value == 0x7f)
            return false;
        // 0x80-0x9f is the C1 range in ISO-8859-x and printable punctuation in
        // CP125x. A sprinkling is fine; a file made of it is not text.
        soft += value >= 0x80 && value <= 0x9f;
    }
    return soft * 20 <= data.size();
}

// The detector scores a decode for *plausibility*; an editor needs the
// stronger guarantee that what it puts on screen can be typed back. A stray
// U+0000 or a decode that is mostly control characters fails that even when
// the byte grammar was satisfied.
bool decodedLooksLikeText(const QString &text) {
    if (text.isEmpty())
        return false;
    qint64 controls = 0;
    for (const QChar character : text) {
        const uint codePoint = character.unicode();
        if (codePoint == 0)
            return false;
        if ((codePoint < 0x20 && codePoint != '\t' && codePoint != '\n' && codePoint != '\r') ||
            (codePoint >= 0x7f && codePoint <= 0x9f))
            ++controls;
    }
    return controls * 10 <= text.size();
}

SniffResult textResult(SniffReason reason, const TextEncodingDetector::Result &detected) {
    SniffResult result;
    result.hex = false;
    result.reason = reason;
    result.encodingLabel = detected.label;
    result.codecName = detected.codecName;
    return result;
}

SniffResult hexResult(SniffReason reason) {
    SniffResult result;
    result.hex = true;
    result.reason = reason;
    return result;
}

} // namespace

int sniffSampleBytes() {
    return kSampleBytes;
}

SniffResult sniff(const QByteArray &sample, const QString &path) {
    // Content-only by contract; see BinarySniff.h.
    Q_UNUSED(path);

    if (sample.isEmpty()) {
        SniffResult result;
        result.reason = SniffReason::EmptyFile;
        result.encodingLabel = QStringLiteral("UTF-8");
        result.codecName = QByteArrayLiteral("UTF-8");
        return result;
    }

    // MayBeTruncated matters: the sample is a prefix, so a multi-byte
    // character cut in half at byte 65536 is a truncation artefact, not
    // evidence of binary content.
    const TextEncodingDetector::Result detected =
        TextEncodingDetector::detect(sample, TextEncodingDetector::InputEnd::MayBeTruncated);

    // NUL bytes and control floods. Note the detector reaches this verdict
    // *after* letting a wide-Unicode candidate rescue the sample, which is why
    // a BOM-less UTF-16 file -- half of whose bytes are NUL -- is still text.
    if (detected.binary)
        return hexResult(SniffReason::DetectorReportedBinary);

    // "Unknown" is the only non-binary verdict that decodes nothing at all:
    // every other branch reports how far it got. Checking the byte count
    // rather than the label keeps this off the detector's display strings.
    if (detected.completePrefixBytes == 0 || detected.codecName.isEmpty()) {
        if (looksLikeSingleByteText(sample)) {
            SniffResult result;
            result.reason = SniffReason::SingleByteFallback;
            result.encodingLabel = detected.label;
            result.codecName = QByteArrayLiteral("UTF-8");
            return result;
        }
        return hexResult(SniffReason::NoEncodingDecodes);
    }

    // The detector sets `ambiguous` when it could not separate its top two
    // candidates. Over bytes containing NULs and with no BOM to anchor the
    // guess, that coin flip decides whether a save re-encodes the file or
    // corrupts it, so it is resolved towards the editor that cannot lose data.
    if (detected.ambiguous && detected.bomBytes == 0 && containsNul(sample))
        return hexResult(SniffReason::AmbiguousWithNulBytes);

    const QByteArray complete =
        TextEncodingDetector::safePrefix(sample, sample.size(), detected);
    if (!decodedLooksLikeText(TextEncodingDetector::decode(complete, detected)))
        return hexResult(SniffReason::ControlCharacterDensity);

    return textResult(SniffReason::DecodedAsText, detected);
}

bool shouldEditAsHex(const QByteArray &sample, const QString &path) {
    return sniff(sample, path).hex;
}

} // namespace fc
