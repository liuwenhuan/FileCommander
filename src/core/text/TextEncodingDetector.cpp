#include "TextEncodingDetector.h"

#include <QChar>
#include <QTextCodec>
#include <QVector>

namespace {

enum class GrammarState { Complete, IncompleteAtEnd, Invalid };

struct GrammarResult {
    GrammarState state = GrammarState::Invalid;
    int completePrefixBytes = 0;
};

enum class ScriptPreference { Chinese, Japanese, Korean };

struct Candidate {
    const char *label;
    const char *codecName;
    ScriptPreference preference;
};

struct UnicodeQuality {
    qint64 codePoints = 0;
    qint64 printable = 0;
    qint64 controls = 0;
    qint64 noncharacters = 0;
    qint64 recognizedScript = 0;
    qint64 nonLatinScript = 0;
    qint64 dominantScript = 0;
    qint64 scripts[5] = {0, 0, 0, 0, 0};
};

struct Score {
    const Candidate *candidate = nullptr;
    qint64 value = 0;
    UnicodeQuality quality;
    GrammarResult grammar;
};

constexpr uchar kAsciiMax = 0x7f;

GrammarResult parseUtf8Grammar(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const int characterStart = i;
        const uchar first = static_cast<uchar>(data.at(i++));
        if (first <= 0x7f)
            continue;

        int continuationCount = 0;
        uint codePoint = 0;
        uint minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuationCount = 1;
            codePoint = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuationCount = 2;
            codePoint = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuationCount = 3;
            codePoint = first & 0x07;
            minimum = 0x10000;
        } else {
            return {GrammarState::Invalid, characterStart};
        }

        for (int j = 0; j < continuationCount; ++j) {
            if (i >= data.size())
                return {GrammarState::IncompleteAtEnd, characterStart};
            const uchar next = static_cast<uchar>(data.at(i++));
            if (next < 0x80 || next > 0xbf)
                return {GrammarState::Invalid, characterStart};
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return {GrammarState::Invalid, characterStart};
    }
    return {GrammarState::Complete, data.size()};
}

GrammarResult parseUtf16Grammar(const QByteArray &data, bool littleEndian) {
    const int alignedBytes = data.size() / 2 * 2;
    auto codeUnitAt = [&data, littleEndian](int offset) {
        const uint first = static_cast<uchar>(data.at(offset));
        const uint second = static_cast<uchar>(data.at(offset + 1));
        return littleEndian ? first | (second << 8) : (first << 8) | second;
    };
    for (int i = 0; i < alignedBytes; i += 2) {
        const uint codeUnit = codeUnitAt(i);
        if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff)
            return {GrammarState::Invalid, i};
        if (codeUnit < 0xd800 || codeUnit > 0xdbff)
            continue;
        if (i + 2 >= alignedBytes)
            return {GrammarState::IncompleteAtEnd, i};
        const uint lowSurrogate = codeUnitAt(i + 2);
        if (lowSurrogate < 0xdc00 || lowSurrogate > 0xdfff)
            return {GrammarState::Invalid, i};
        i += 2;
    }
    if (alignedBytes != data.size())
        return {GrammarState::IncompleteAtEnd, alignedBytes};
    return {GrammarState::Complete, data.size()};
}

GrammarResult parseUtf32Grammar(const QByteArray &data, bool littleEndian) {
    const int alignedBytes = data.size() / 4 * 4;
    for (int i = 0; i < alignedBytes; i += 4) {
        uint codePoint = 0;
        for (int byte = 0; byte < 4; ++byte) {
            const uint value = static_cast<uchar>(data.at(i + byte));
            codePoint |= littleEndian ? value << (byte * 8) : value << ((3 - byte) * 8);
        }
        if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return {GrammarState::Invalid, i};
    }
    if (alignedBytes != data.size())
        return {GrammarState::IncompleteAtEnd, alignedBytes};
    return {GrammarState::Complete, data.size()};
}

bool isAscii(const QByteArray &data) {
    for (char byte : data)
        if (static_cast<uchar>(byte) > kAsciiMax)
            return false;
    return true;
}

bool isStrictUtf8(const QByteArray &data) {
    return parseUtf8Grammar(data).state == GrammarState::Complete;
}

bool isStrictUtf16(const QByteArray &data, bool littleEndian) {
    return parseUtf16Grammar(data, littleEndian).state == GrammarState::Complete;
}

bool isStrictUtf32(const QByteArray &data, bool littleEndian) {
    return parseUtf32Grammar(data, littleEndian).state == GrammarState::Complete;
}

bool isNoncharacter(uint codePoint) {
    return (codePoint >= 0xfdd0 && codePoint <= 0xfdef) ||
           ((codePoint & 0xfffe) == 0xfffe && codePoint <= 0x10ffff);
}

bool isControl(uint codePoint) {
    return (codePoint < 0x20 && codePoint != '\t' && codePoint != '\n' && codePoint != '\r') ||
           (codePoint >= 0x7f && codePoint <= 0x9f);
}

enum class UnicodeScript { Latin, Cyrillic, Han, Kana, Hangul, Other };

UnicodeScript unicodeScript(uint codePoint) {
    if ((codePoint >= 0x0041 && codePoint <= 0x024f) ||
        (codePoint >= 0x1e00 && codePoint <= 0x1eff))
        return UnicodeScript::Latin;
    if (codePoint >= 0x0400 && codePoint <= 0x052f)
        return UnicodeScript::Cyrillic;
    if ((codePoint >= 0x3400 && codePoint <= 0x4dbf) ||
        (codePoint >= 0x4e00 && codePoint <= 0x9fff) ||
        (codePoint >= 0xf900 && codePoint <= 0xfaff))
        return UnicodeScript::Han;
    if ((codePoint >= 0x3040 && codePoint <= 0x30ff) ||
        (codePoint >= 0x31f0 && codePoint <= 0x31ff))
        return UnicodeScript::Kana;
    if ((codePoint >= 0x1100 && codePoint <= 0x11ff) ||
        (codePoint >= 0xac00 && codePoint <= 0xd7af))
        return UnicodeScript::Hangul;
    return UnicodeScript::Other;
}

void observeCodePoint(UnicodeQuality *quality, uint codePoint) {
    ++quality->codePoints;
    quality->controls += isControl(codePoint);
    quality->noncharacters += isNoncharacter(codePoint);
    quality->printable += QChar::isPrint(codePoint) || QChar::isSpace(codePoint);

    const UnicodeScript script = unicodeScript(codePoint);
    if (script != UnicodeScript::Other) {
        const int index = static_cast<int>(script);
        ++quality->recognizedScript;
        quality->nonLatinScript += script != UnicodeScript::Latin;
        ++quality->scripts[index];
        quality->dominantScript = qMax(quality->dominantScript, quality->scripts[index]);
    }
}

struct WideCandidate {
    const char *label = nullptr;
    const char *codecName = nullptr;
    UnicodeQuality quality;
    qint64 score = -1;
    bool structuralEvidence = false;
    GrammarResult grammar;
};

qint64 scriptSpecificity(const UnicodeQuality &quality) {
    return quality.scripts[static_cast<int>(UnicodeScript::Latin)] +
           quality.scripts[static_cast<int>(UnicodeScript::Cyrillic)] * 4 +
           quality.scripts[static_cast<int>(UnicodeScript::Han)] * 2 +
           quality.scripts[static_cast<int>(UnicodeScript::Kana)] * 4 +
           quality.scripts[static_cast<int>(UnicodeScript::Hangul)] * 4;
}

bool hasTextQuality(const UnicodeQuality &quality, int minimumCodePoints,
                    bool structuralEvidence = false) {
    if (quality.codePoints < minimumCodePoints || quality.controls != 0 ||
        quality.noncharacters != 0 || quality.printable * 10 < quality.codePoints * 9)
        return false;
    // A stable byte layout is strong wide-Unicode evidence. Without it, require
    // non-Latin script to dominate the meaningful characters while allowing
    // normal punctuation and whitespace in prose.
    return structuralEvidence ||
           (quality.nonLatinScript >= minimumCodePoints &&
            quality.nonLatinScript * 4 >= quality.recognizedScript * 3 &&
            quality.nonLatinScript * 2 >= quality.codePoints);
}

bool hasStableZeroLanes(const QByteArray &data, int codeUnitBytes, bool littleEndian) {
    const qint64 codeUnits = data.size() / codeUnitBytes;
    if (codeUnits < 4)
        return false;

    const QVector<int> highLanes =
        codeUnitBytes == 2 ? QVector<int>{littleEndian ? 1 : 0}
                           : (littleEndian ? QVector<int>{1, 2, 3} : QVector<int>{0, 1, 2});
    int zeroLanes = 0;
    for (const int lane : highLanes) {
        qint64 zeroCount = 0;
        for (int offset = lane; offset < data.size(); offset += codeUnitBytes)
            zeroCount += static_cast<uchar>(data.at(offset)) == 0;
        zeroLanes += zeroCount * 4 >= codeUnits * 3;
    }
    // BMP UTF-32 commonly has two zero lanes; ASCII has all three.
    if (zeroLanes >= (codeUnitBytes == 2 ? 1 : 2))
        return true;

    if (codeUnitBytes != 2)
        return false;

    // Hiragana and Katakana occupy compact blocks whose UTF-16 high byte stays
    // constant (normally 0x30). This is also useful structure even though the
    // byte stream itself happens to be printable ASCII.
    const int lane = littleEndian ? 1 : 0;
    const uchar expected = static_cast<uchar>(data.at(lane));
    if (expected == 0 || expected > 0x7f)
        return false;
    qint64 matching = 0;
    for (int offset = lane; offset < data.size(); offset += codeUnitBytes)
        matching += static_cast<uchar>(data.at(offset)) == expected;
    return matching * 4 >= codeUnits * 3;
}

qint64 unicodeQualityScore(const UnicodeQuality &quality, bool structuralEvidence) {
    if (!hasTextQuality(quality, 4, structuralEvidence))
        return -1;
    return scriptSpecificity(quality) * 20 + quality.printable * 2 +
           (structuralEvidence ? 100 : 0);
}

bool hasHighQualityUtf8(const QByteArray &data) {
    if (!isStrictUtf8(data))
        return false;
    UnicodeQuality quality;
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i++));
        uint codePoint = first;
        int continuationCount = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codePoint &= 0x1f;
            continuationCount = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codePoint &= 0x0f;
            continuationCount = 2;
        } else if (first >= 0xf0) {
            codePoint &= 0x07;
            continuationCount = 3;
        }
        for (int j = 0; j < continuationCount; ++j)
            codePoint = (codePoint << 6) | (static_cast<uchar>(data.at(i++)) & 0x3f);
        observeCodePoint(&quality, codePoint);
    }
    return quality.codePoints >= 1 && quality.controls == 0 && quality.noncharacters == 0 &&
           quality.printable * 4 >= quality.codePoints * 3;
}

qint64 utf16QualityScore(const QByteArray &data, bool littleEndian,
                      UnicodeQuality *observedQuality = nullptr,
                      bool *structuralEvidence = nullptr) {
    if (!isStrictUtf16(data, littleEndian))
        return -1;

    auto codeUnitAt = [&data, littleEndian](int offset) {
        const uint first = static_cast<uchar>(data.at(offset));
        const uint second = static_cast<uchar>(data.at(offset + 1));
        return littleEndian ? first | (second << 8) : (first << 8) | second;
    };
    UnicodeQuality quality;
    for (int i = 0; i < data.size(); i += 2) {
        uint codePoint = codeUnitAt(i);
        if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
            codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (codeUnitAt(i + 2) - 0xdc00);
            i += 2;
        }
        observeCodePoint(&quality, codePoint);
    }
    const bool structural = hasStableZeroLanes(data, 2, littleEndian);
    if (observedQuality)
        *observedQuality = quality;
    if (structuralEvidence)
        *structuralEvidence = structural;
    return unicodeQualityScore(quality, structural);
}

qint64 utf32QualityScore(const QByteArray &data, bool littleEndian,
                      UnicodeQuality *observedQuality = nullptr,
                      bool *structuralEvidence = nullptr) {
    if (!isStrictUtf32(data, littleEndian))
        return -1;

    UnicodeQuality quality;
    for (int i = 0; i < data.size(); i += 4) {
        uint codePoint = 0;
        for (int byte = 0; byte < 4; ++byte) {
            const uint value = static_cast<uchar>(data.at(i + byte));
            codePoint |= littleEndian ? value << (byte * 8) : value << ((3 - byte) * 8);
        }
        observeCodePoint(&quality, codePoint);
    }
    const bool structural = hasStableZeroLanes(data, 4, littleEndian);
    if (observedQuality)
        *observedQuality = quality;
    if (structuralEvidence)
        *structuralEvidence = structural;
    return unicodeQualityScore(quality, structural);
}

qint64 scriptQuality(const QString &text, ScriptPreference preference) {
    qint64 score = 0;
    for (const QChar character : text) {
        const uint codePoint = character.unicode();
        const bool han = character.isHighSurrogate() || character.isLowSurrogate()
                             ? false
                             : ((codePoint >= 0x3400 && codePoint <= 0x4dbf) ||
                                (codePoint >= 0x4e00 && codePoint <= 0x9fff) ||
                                (codePoint >= 0xf900 && codePoint <= 0xfaff));
        const bool kana = (codePoint >= 0x3040 && codePoint <= 0x30ff) ||
                          (codePoint >= 0x31f0 && codePoint <= 0x31ff);
        const bool hangul = (codePoint >= 0x1100 && codePoint <= 0x11ff) ||
                            (codePoint >= 0xac00 && codePoint <= 0xd7af);
        switch (preference) {
        case ScriptPreference::Chinese:
            score += han ? 8 : 0;
            score -= kana ? 4 : 0;
            score -= hangul ? 6 : 0;
            break;
        case ScriptPreference::Japanese:
            score += han ? 1 : 0;
            score += kana ? 25 : 0;
            score -= hangul ? 6 : 0;
            break;
        case ScriptPreference::Korean:
            score += hangul ? 15 : 0;
            score += han ? 1 : 0;
            score -= kana ? 6 : 0;
            break;
        }
    }
    return score;
}

qint64 scoreDecodedText(const QByteArray &data, QTextCodec *codec,
                        ScriptPreference preference,
                        UnicodeQuality *observedQuality = nullptr) {
    const QString decoded = codec->toUnicode(data);
    UnicodeQuality quality;
    qint64 replacements = 0;
    for (const QChar character : decoded) {
        const uint codePoint = character.unicode();
        replacements += codePoint == 0xfffd;
        observeCodePoint(&quality, codePoint);
    }

    qint64 score = quality.printable * 4 + scriptQuality(decoded, preference);
    score -= replacements * 50;
    score -= quality.controls * 12;
    score -= quality.noncharacters * 25;
    if (codec->fromUnicode(decoded) != data)
        score -= 20;
    if (observedQuality)
        *observedQuality = quality;
    return score;
}

bool isLikelyBinary(const QByteArray &data) {
    qint64 nulls = 0;
    qint64 controls = 0;
    for (char byte : data) {
        const uchar value = static_cast<uchar>(byte);
        nulls += value == 0;
        controls += value < 0x20 && value != '\t' && value != '\n' && value != '\r';
    }
    return nulls > 0 || (data.size() >= 8 && controls * 4 > data.size());
}

GrammarResult parseLegacyGrammar(const QByteArray &data, const QByteArray &codecName) {
    int i = 0;
    while (i < data.size()) {
        const int characterStart = i;
        const uchar first = static_cast<uchar>(data.at(i++));
        if (first <= 0x7f)
            continue;

        auto incomplete = [characterStart]() {
            return GrammarResult{GrammarState::IncompleteAtEnd, characterStart};
        };
        auto invalid = [characterStart]() {
            return GrammarResult{GrammarState::Invalid, characterStart};
        };

        if (codecName == "GB18030") {
            if (first < 0x81 || first > 0xfe)
                return invalid();
            if (i >= data.size())
                return incomplete();
            const uchar second = static_cast<uchar>(data.at(i++));
            if (second >= 0x30 && second <= 0x39) {
                if (i >= data.size())
                    return incomplete();
                const uchar third = static_cast<uchar>(data.at(i++));
                if (third < 0x81 || third > 0xfe)
                    return invalid();
                if (i >= data.size())
                    return incomplete();
                const uchar fourth = static_cast<uchar>(data.at(i++));
                if (fourth < 0x30 || fourth > 0x39)
                    return invalid();
            } else if ((second < 0x40 || second > 0xfe) || second == 0x7f) {
                return invalid();
            }
        } else if (codecName == "Big5") {
            if (first < 0x81 || first > 0xfe)
                return invalid();
            if (i >= data.size())
                return incomplete();
            const uchar second = static_cast<uchar>(data.at(i++));
            if (!((second >= 0x40 && second <= 0x7e) ||
                  (second >= 0xa1 && second <= 0xfe)))
                return invalid();
        } else if (codecName == "Shift-JIS") {
            if (first >= 0xa1 && first <= 0xdf)
                continue;
            if (!((first >= 0x81 && first <= 0x9f) ||
                  (first >= 0xe0 && first <= 0xfc)))
                return invalid();
            if (i >= data.size())
                return incomplete();
            const uchar second = static_cast<uchar>(data.at(i++));
            if (!((second >= 0x40 && second <= 0x7e) ||
                  (second >= 0x80 && second <= 0xfc)))
                return invalid();
        } else if (codecName == "EUC-JP") {
            if (first == 0x8e) {
                if (i >= data.size())
                    return incomplete();
                const uchar second = static_cast<uchar>(data.at(i++));
                if (second < 0xa1 || second > 0xdf)
                    return invalid();
            } else if (first == 0x8f) {
                if (i >= data.size())
                    return incomplete();
                const uchar second = static_cast<uchar>(data.at(i++));
                if (second < 0xa1 || second > 0xfe)
                    return invalid();
                if (i >= data.size())
                    return incomplete();
                const uchar third = static_cast<uchar>(data.at(i++));
                if (third < 0xa1 || third > 0xfe)
                    return invalid();
            } else {
                if (first < 0xa1 || first > 0xfe)
                    return invalid();
                if (i >= data.size())
                    return incomplete();
                const uchar second = static_cast<uchar>(data.at(i++));
                if (second < 0xa1 || second > 0xfe)
                    return invalid();
            }
        } else if (codecName == "EUC-KR") {
            if (first < 0xa1 || first > 0xfe)
                return invalid();
            if (i >= data.size())
                return incomplete();
            const uchar second = static_cast<uchar>(data.at(i++));
            if (second < 0xa1 || second > 0xfe)
                return invalid();
        } else {
            return invalid();
        }
    }
    return {GrammarState::Complete, data.size()};
}

int longestCompleteLegacyPrefix(const QByteArray &data, int length, const QByteArray &codecName) {
    return parseLegacyGrammar(data.left(length), codecName).completePrefixBytes;
}

TextEncodingDetector::Result bomResult(const char *label, const char *codecName, int bomBytes,
                                       const GrammarResult &grammar, bool incompleteTail) {
    return {QString::fromLatin1(label), QByteArray(codecName), bomBytes, false, false,
            bomBytes + grammar.completePrefixBytes, incompleteTail};
}

bool acceptsGrammar(const GrammarResult &grammar, TextEncodingDetector::InputEnd inputEnd) {
    return grammar.state == GrammarState::Complete ||
           (inputEnd == TextEncodingDetector::InputEnd::MayBeTruncated &&
            grammar.state == GrammarState::IncompleteAtEnd);
}

bool hasDecodedTextQuality(const UnicodeQuality &quality) {
    return quality.codePoints > 0 && quality.noncharacters == 0 &&
           quality.controls * 10 <= quality.codePoints &&
           quality.printable * 10 >= quality.codePoints * 9;
}

bool hasStrongLegacyEvidence(const UnicodeQuality &quality) {
    return hasDecodedTextQuality(quality) &&
           quality.nonLatinScript * 4 >= quality.codePoints * 3;
}

} // namespace

TextEncodingDetector::Result TextEncodingDetector::detect(const QByteArray &data,
                                                           InputEnd inputEnd) {
    const auto unknown = []() {
        return Result{QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true,
                      0, false};
    };

    // UTF-32 prefixes contain the UTF-16 BOM prefixes, so UTF-32 must win first.
    if (data.startsWith(QByteArrayLiteral("\xFF\xFE\x00\x00"))) {
        const GrammarResult grammar = parseUtf32Grammar(data.mid(4), true);
        if (acceptsGrammar(grammar, inputEnd))
            return bomResult("UTF-32LE", "UTF-32LE", 4, grammar,
                             grammar.state == GrammarState::IncompleteAtEnd);
        return unknown();
    }
    if (data.startsWith(QByteArrayLiteral("\x00\x00\xFE\xFF"))) {
        const GrammarResult grammar = parseUtf32Grammar(data.mid(4), false);
        if (acceptsGrammar(grammar, inputEnd))
            return bomResult("UTF-32BE", "UTF-32BE", 4, grammar,
                             grammar.state == GrammarState::IncompleteAtEnd);
        return unknown();
    }
    if (data.startsWith(QByteArrayLiteral("\xEF\xBB\xBF"))) {
        const GrammarResult grammar = parseUtf8Grammar(data.mid(3));
        if (acceptsGrammar(grammar, inputEnd))
            return bomResult("UTF-8", "UTF-8", 3, grammar,
                             grammar.state == GrammarState::IncompleteAtEnd);
        return unknown();
    }
    if (data.startsWith(QByteArrayLiteral("\xFF\xFE"))) {
        const GrammarResult grammar = parseUtf16Grammar(data.mid(2), true);
        if (acceptsGrammar(grammar, inputEnd))
            return bomResult("UTF-16LE", "UTF-16LE", 2, grammar,
                             grammar.state == GrammarState::IncompleteAtEnd);
        return unknown();
    }
    if (data.startsWith(QByteArrayLiteral("\xFE\xFF"))) {
        const GrammarResult grammar = parseUtf16Grammar(data.mid(2), false);
        if (acceptsGrammar(grammar, inputEnd))
            return bomResult("UTF-16BE", "UTF-16BE", 2, grammar,
                             grammar.state == GrammarState::IncompleteAtEnd);
        return unknown();
    }

    // Strict non-ASCII UTF-8 is stronger evidence than a coincidental UTF-16 pairing.
    // A truncated probe is accepted only when the observed prefix is itself
    // high-quality UTF-8 and the sole error is the physical tail.
    const GrammarResult utf8Grammar = parseUtf8Grammar(data);
    const bool acceptedIncompleteUtf8 =
        inputEnd == InputEnd::MayBeTruncated &&
        utf8Grammar.state == GrammarState::IncompleteAtEnd &&
        utf8Grammar.completePrefixBytes > 0 &&
        !isAscii(data.left(utf8Grammar.completePrefixBytes)) &&
        hasHighQualityUtf8(data.left(utf8Grammar.completePrefixBytes));
    if ((!isAscii(data) && utf8Grammar.state == GrammarState::Complete &&
         hasHighQualityUtf8(data)) ||
        acceptedIncompleteUtf8) {
        return {QStringLiteral("UTF-8"), QByteArrayLiteral("UTF-8"), 0, false, false,
                utf8Grammar.completePrefixBytes, acceptedIncompleteUtf8};
    }

    QVector<WideCandidate> wideCandidates;
    auto appendWide = [&wideCandidates, inputEnd](const char *label, const char *codecName,
                                                   const QByteArray &completeData,
                                                   const GrammarResult &grammar,
                                                   bool littleEndian, int codeUnitBytes,
                                                   bool oppositeEndianInvalid = false) {
        const bool acceptedIncomplete = inputEnd == InputEnd::MayBeTruncated &&
                                        grammar.state == GrammarState::IncompleteAtEnd &&
                                        grammar.completePrefixBytes > 0;
        if (grammar.state != GrammarState::Complete && !acceptedIncomplete)
            return;

        UnicodeQuality quality;
        bool structural = false;
        const qint64 score = codeUnitBytes == 4
                              ? utf32QualityScore(completeData, littleEndian, &quality, &structural)
                              : utf16QualityScore(completeData, littleEndian, &quality, &structural);
        if (score >= 0 && (structural || !oppositeEndianInvalid))
            wideCandidates.append({label, codecName, quality, score, structural, grammar});
    };

    const GrammarResult utf32LeGrammar = parseUtf32Grammar(data, true);
    appendWide("UTF-32LE", "UTF-32LE", data.left(utf32LeGrammar.completePrefixBytes),
               utf32LeGrammar, true, 4);
    const GrammarResult utf32BeGrammar = parseUtf32Grammar(data, false);
    appendWide("UTF-32BE", "UTF-32BE", data.left(utf32BeGrammar.completePrefixBytes),
               utf32BeGrammar, false, 4);
    const GrammarResult utf16LeGrammar = parseUtf16Grammar(data, true);
    const GrammarResult utf16BeGrammar = parseUtf16Grammar(data, false);
    appendWide("UTF-16LE", "UTF-16LE", data.left(utf16LeGrammar.completePrefixBytes),
               utf16LeGrammar, true, 2, utf16BeGrammar.state == GrammarState::Invalid);
    appendWide("UTF-16BE", "UTF-16BE", data.left(utf16BeGrammar.completePrefixBytes),
               utf16BeGrammar, false, 2, utf16LeGrammar.state == GrammarState::Invalid);

    int bestWide = -1;
    int secondWide = -1;
    for (int i = 0; i < wideCandidates.size(); ++i) {
        if (bestWide < 0 || wideCandidates.at(i).score > wideCandidates.at(bestWide).score) {
            secondWide = bestWide;
            bestWide = i;
        } else if (secondWide < 0 ||
                   wideCandidates.at(i).score > wideCandidates.at(secondWide).score) {
            secondWide = i;
        }
    }

    const bool likelyBinary = isLikelyBinary(data);
    if (likelyBinary && bestWide >= 0) {
        const WideCandidate &winner = wideCandidates.at(bestWide);
        const bool closeWide = secondWide >= 0 &&
                               winner.score - wideCandidates.at(secondWide).score <= 6;
        return {QString::fromLatin1(winner.label), QByteArray(winner.codecName), 0, false,
                closeWide, winner.grammar.completePrefixBytes,
                winner.grammar.state == GrammarState::IncompleteAtEnd};
    }

    if (likelyBinary)
        return {QStringLiteral("Binary"), QByteArray(), 0, true, false, 0, false};

    const Candidate candidates[] = {
        {"GB18030", "GB18030", ScriptPreference::Chinese},
        {"Big5", "Big5", ScriptPreference::Chinese},
        {"Shift-JIS", "Shift-JIS", ScriptPreference::Japanese},
        {"EUC-JP", "EUC-JP", ScriptPreference::Japanese},
        {"EUC-KR", "EUC-KR", ScriptPreference::Korean},
    };

    QVector<Score> scores;
    for (const Candidate &candidate : candidates) {
        const GrammarResult grammar = parseLegacyGrammar(data, candidate.codecName);
        const bool acceptedIncomplete = inputEnd == InputEnd::MayBeTruncated &&
                                        grammar.state == GrammarState::IncompleteAtEnd &&
                                        grammar.completePrefixBytes > 0;
        if (grammar.state != GrammarState::Complete && !acceptedIncomplete)
            continue;
        QTextCodec *codec = QTextCodec::codecForName(candidate.codecName);
        if (!codec)
            continue;
        const int sampleLimit = qMin(grammar.completePrefixBytes,
                                     TextEncodingDetector::legacyScoreSampleBytes());
        const int sampleBytes = longestCompleteLegacyPrefix(data, sampleLimit,
                                                             candidate.codecName);
        UnicodeQuality decodedQuality;
        const qint64 value = scoreDecodedText(data.left(sampleBytes), codec,
                                           candidate.preference, &decodedQuality);
        if (hasDecodedTextQuality(decodedQuality))
            scores.append({&candidate, value, decodedQuality, grammar});
    }

    int bestLegacy = -1;
    int secondLegacy = -1;
    for (int i = 0; i < scores.size(); ++i) {
        if (bestLegacy < 0 || scores.at(i).value > scores.at(bestLegacy).value) {
            secondLegacy = bestLegacy;
            bestLegacy = i;
        } else if (secondLegacy < 0 || scores.at(i).value > scores.at(secondLegacy).value) {
            secondLegacy = i;
        }
    }

    const bool strongLegacy = bestLegacy >= 0 &&
                              hasStrongLegacyEvidence(scores.at(bestLegacy).quality);
    if (bestWide >= 0 &&
        (wideCandidates.at(bestWide).structuralEvidence || !strongLegacy)) {
        const WideCandidate &winner = wideCandidates.at(bestWide);
        const bool closeWide = secondWide >= 0 &&
                               winner.score - wideCandidates.at(secondWide).score <= 6;
        return {QString::fromLatin1(winner.label), QByteArray(winner.codecName), 0, false,
                closeWide || bestLegacy >= 0, winner.grammar.completePrefixBytes,
                winner.grammar.state == GrammarState::IncompleteAtEnd};
    }

    if (isAscii(data) && !isLikelyBinary(data))
        return {QStringLiteral("ASCII"), QByteArrayLiteral("UTF-8"), 0, false,
                data.isEmpty(), data.size(), false};

    if (bestLegacy >= 0) {
        const Score &winner = scores.at(bestLegacy);
        const bool closeScores = secondLegacy >= 0 &&
                                 winner.value - scores.at(secondLegacy).value <= 6;
        const bool shortSample = winner.grammar.completePrefixBytes < 8;
        return {QString::fromLatin1(winner.candidate->label),
                QByteArray(winner.candidate->codecName), 0, false,
                shortSample || closeScores || bestWide >= 0,
                winner.grammar.completePrefixBytes,
                winner.grammar.state == GrammarState::IncompleteAtEnd};
    }

    return unknown();
}

QString TextEncodingDetector::decode(const QByteArray &data, const Result &result) {
    if (result.binary || result.codecName.isEmpty())
        return QString();
    QTextCodec *codec = QTextCodec::codecForName(result.codecName);
    if (!codec)
        return QString::fromUtf8(data.mid(result.bomBytes));
    return codec->toUnicode(data.mid(result.bomBytes));
}

QByteArray TextEncodingDetector::safePrefix(const QByteArray &data, int maximumBytes,
                                             const Result &result) {
    if (maximumBytes <= 0 || maximumBytes < result.bomBytes)
        return QByteArray();

    int length = qMin(data.size(), maximumBytes);
    if (result.incompleteTail)
        length = qMin(length, result.completePrefixBytes);
    if (data.size() <= maximumBytes && !result.incompleteTail)
        return data;

    const int payloadStart = qMin(result.bomBytes, length);
    if (result.codecName == "UTF-32LE" || result.codecName == "UTF-32BE") {
        length = payloadStart + (length - payloadStart) / 4 * 4;
    } else if (result.codecName == "UTF-16LE" || result.codecName == "UTF-16BE") {
        length = payloadStart + (length - payloadStart) / 2 * 2;
        if (length - payloadStart >= 2) {
            const bool littleEndian = result.codecName == "UTF-16LE";
            const uchar first = static_cast<uchar>(data.at(length - 2));
            const uchar second = static_cast<uchar>(data.at(length - 1));
            const uint codeUnit = littleEndian ? first | (second << 8) : (first << 8) | second;
            if (codeUnit >= 0xd800 && codeUnit <= 0xdbff)
                length -= 2;
        }
    } else if (result.codecName == "UTF-8") {
        if (length > payloadStart) {
            int sequenceStart = length - 1;
            while (sequenceStart > payloadStart &&
                   (static_cast<uchar>(data.at(sequenceStart)) & 0xc0) == 0x80)
                --sequenceStart;
            const uchar first = static_cast<uchar>(data.at(sequenceStart));
            const int sequenceBytes = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            if (sequenceStart + sequenceBytes > length)
                length = sequenceStart;
        }
    } else if (!result.codecName.isEmpty()) {
        // Legacy encodings are variable-width. Re-run only the selected codec's
        // byte grammar once to locate the boundary; no decode/re-encode and no
        // candidate re-detection is performed for adjacent large prefixes.
        length = longestCompleteLegacyPrefix(data, length, result.codecName);
    }
    return data.left(qMax(payloadStart, length));
}
