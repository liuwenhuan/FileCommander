#include "TextEncodingDetector.h"

#include <QChar>
#include <QTextCodec>
#include <QVector>

namespace {

enum class ScriptPreference { Chinese, Japanese, Korean };

struct Candidate {
    const char *label;
    const char *codecName;
    ScriptPreference preference;
    bool (*hasValidGrammar)(const QByteArray &);
};

struct Score {
    const Candidate *candidate = nullptr;
    int value = 0;
};

constexpr uchar kAsciiMax = 0x7f;

bool isAscii(const QByteArray &data) {
    for (char byte : data)
        if (static_cast<uchar>(byte) > kAsciiMax)
            return false;
    return true;
}

bool isStrictUtf8(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i));
        if (first <= 0x7f) {
            ++i;
            continue;
        }

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
            return false;
        }

        if (i + continuationCount >= data.size())
            return false;
        for (int j = 1; j <= continuationCount; ++j) {
            const uchar next = static_cast<uchar>(data.at(i + j));
            if (next < 0x80 || next > 0xbf)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
        i += continuationCount + 1;
    }
    return true;
}

bool isStrictUtf16(const QByteArray &data, bool littleEndian) {
    if (data.size() % 2 != 0)
        return false;
    auto codeUnitAt = [&data, littleEndian](int offset) {
        const uint first = static_cast<uchar>(data.at(offset));
        const uint second = static_cast<uchar>(data.at(offset + 1));
        return littleEndian ? first | (second << 8) : (first << 8) | second;
    };
    for (int i = 0; i < data.size(); i += 2) {
        const uint codeUnit = codeUnitAt(i);
        if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff)
            return false;
        if (codeUnit < 0xd800 || codeUnit > 0xdbff)
            continue;
        if (i + 2 >= data.size())
            return false;
        const uint lowSurrogate = codeUnitAt(i + 2);
        if (lowSurrogate < 0xdc00 || lowSurrogate > 0xdfff)
            return false;
        i += 2;
    }
    return true;
}

bool isStrictUtf32(const QByteArray &data, bool littleEndian) {
    if (data.size() % 4 != 0)
        return false;
    for (int i = 0; i < data.size(); i += 4) {
        uint codePoint = 0;
        for (int byte = 0; byte < 4; ++byte) {
            const uint value = static_cast<uchar>(data.at(i + byte));
            codePoint |= littleEndian ? value << (byte * 8) : value << ((3 - byte) * 8);
        }
        if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
    }
    return true;
}

bool isGb18030(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i));
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first < 0x81 || first > 0xfe || ++i >= data.size())
            return false;

        const uchar second = static_cast<uchar>(data.at(i));
        if (second >= 0x30 && second <= 0x39) {
            if (i + 2 >= data.size())
                return false;
            const uchar third = static_cast<uchar>(data.at(i + 1));
            const uchar fourth = static_cast<uchar>(data.at(i + 2));
            if (third < 0x81 || third > 0xfe || fourth < 0x30 || fourth > 0x39)
                return false;
            i += 3;
            continue;
        }
        if ((second < 0x40 || second > 0xfe) || second == 0x7f)
            return false;
        ++i;
    }
    return true;
}

bool isBig5(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i));
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first < 0x81 || first > 0xfe || ++i >= data.size())
            return false;
        const uchar second = static_cast<uchar>(data.at(i++));
        if (!((second >= 0x40 && second <= 0x7e) || (second >= 0xa1 && second <= 0xfe)))
            return false;
    }
    return true;
}

bool isShiftJis(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i));
        if (first <= 0x7f || (first >= 0xa1 && first <= 0xdf)) {
            ++i;
            continue;
        }
        if (!((first >= 0x81 && first <= 0x9f) || (first >= 0xe0 && first <= 0xfc)) ||
            ++i >= data.size())
            return false;
        const uchar second = static_cast<uchar>(data.at(i++));
        if (!((second >= 0x40 && second <= 0x7e) || (second >= 0x80 && second <= 0xfc)))
            return false;
    }
    return true;
}

bool isEucJp(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i));
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first == 0x8e) {
            if (++i >= data.size())
                return false;
            const uchar second = static_cast<uchar>(data.at(i++));
            if (second < 0xa1 || second > 0xdf)
                return false;
            continue;
        }
        if (first == 0x8f) {
            if (i + 2 >= data.size())
                return false;
            const uchar second = static_cast<uchar>(data.at(i + 1));
            const uchar third = static_cast<uchar>(data.at(i + 2));
            if (second < 0xa1 || second > 0xfe || third < 0xa1 || third > 0xfe)
                return false;
            i += 3;
            continue;
        }
        if (first < 0xa1 || first > 0xfe || ++i >= data.size())
            return false;
        const uchar second = static_cast<uchar>(data.at(i++));
        if (second < 0xa1 || second > 0xfe)
            return false;
    }
    return true;
}

bool isEucKr(const QByteArray &data) {
    for (int i = 0; i < data.size();) {
        const uchar first = static_cast<uchar>(data.at(i));
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first < 0xa1 || first > 0xfe || ++i >= data.size())
            return false;
        const uchar second = static_cast<uchar>(data.at(i++));
        if (second < 0xa1 || second > 0xfe)
            return false;
    }
    return true;
}

bool isNoncharacter(uint codePoint) {
    return (codePoint >= 0xfdd0 && codePoint <= 0xfdef) ||
           ((codePoint & 0xfffe) == 0xfffe && codePoint <= 0x10ffff);
}

bool isControl(uint codePoint) {
    return (codePoint < 0x20 && codePoint != '\t' && codePoint != '\n' && codePoint != '\r') ||
           (codePoint >= 0x7f && codePoint <= 0x9f);
}

int scriptQuality(const QString &text, ScriptPreference preference) {
    int score = 0;
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

int scoreDecodedText(const QByteArray &data, QTextCodec *codec, ScriptPreference preference) {
    const QString decoded = codec->toUnicode(data);
    int printable = 0;
    int replacements = 0;
    int controls = 0;
    int noncharacters = 0;
    for (const QChar character : decoded) {
        const uint codePoint = character.unicode();
        replacements += codePoint == 0xfffd;
        controls += isControl(codePoint);
        noncharacters += isNoncharacter(codePoint);
        printable += QChar::isPrint(codePoint) || character.isSpace();
    }

    int score = printable * 4 + scriptQuality(decoded, preference);
    score -= replacements * 50;
    score -= controls * 12;
    score -= noncharacters * 25;
    if (codec->fromUnicode(decoded) != data)
        score -= 20;
    return score;
}

bool isLikelyBinary(const QByteArray &data) {
    int nulls = 0;
    int controls = 0;
    for (char byte : data) {
        const uchar value = static_cast<uchar>(byte);
        nulls += value == 0;
        controls += value < 0x20 && value != '\t' && value != '\n' && value != '\r';
    }
    return nulls > 0 || (data.size() >= 8 && controls * 4 > data.size());
}

TextEncodingDetector::Result bomResult(const char *label, const char *codecName, int bomBytes) {
    return {QString::fromLatin1(label), QByteArray(codecName), bomBytes, false, false};
}

} // namespace

TextEncodingDetector::Result TextEncodingDetector::detect(const QByteArray &data) {
    // UTF-32 prefixes contain the UTF-16 BOM prefixes, so UTF-32 must win first.
    if (data.startsWith(QByteArrayLiteral("\xFF\xFE\x00\x00"))) {
        if (isStrictUtf32(data.mid(4), true))
            return bomResult("UTF-32LE", "UTF-32LE", 4);
        return {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};
    }
    if (data.startsWith(QByteArrayLiteral("\x00\x00\xFE\xFF"))) {
        if (isStrictUtf32(data.mid(4), false))
            return bomResult("UTF-32BE", "UTF-32BE", 4);
        return {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};
    }
    if (data.startsWith(QByteArrayLiteral("\xEF\xBB\xBF"))) {
        if (isStrictUtf8(data.mid(3)))
            return bomResult("UTF-8", "UTF-8", 3);
        return {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};
    }
    if (data.startsWith(QByteArrayLiteral("\xFF\xFE"))) {
        if (isStrictUtf16(data.mid(2), true))
            return bomResult("UTF-16LE", "UTF-16LE", 2);
        return {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};
    }
    if (data.startsWith(QByteArrayLiteral("\xFE\xFF"))) {
        if (isStrictUtf16(data.mid(2), false))
            return bomResult("UTF-16BE", "UTF-16BE", 2);
        return {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};
    }

    if (isLikelyBinary(data))
        return {QStringLiteral("Binary"), QByteArray(), 0, true, false};
    if (isAscii(data))
        return {QStringLiteral("ASCII"), QByteArrayLiteral("UTF-8"), 0, false, data.isEmpty()};
    if (isStrictUtf8(data))
        return {QStringLiteral("UTF-8"), QByteArrayLiteral("UTF-8"), 0, false, false};

    // Order is the documented deterministic tie-break order after UTF-8.
    const Candidate candidates[] = {
        {"GB18030", "GB18030", ScriptPreference::Chinese, isGb18030},
        {"Big5", "Big5", ScriptPreference::Chinese, isBig5},
        {"Shift-JIS", "Shift-JIS", ScriptPreference::Japanese, isShiftJis},
        {"EUC-JP", "EUC-JP", ScriptPreference::Japanese, isEucJp},
        {"EUC-KR", "EUC-KR", ScriptPreference::Korean, isEucKr},
    };

    QVector<Score> scores;
    for (const Candidate &candidate : candidates) {
        if (!candidate.hasValidGrammar(data))
            continue;
        QTextCodec *codec = QTextCodec::codecForName(candidate.codecName);
        if (!codec)
            continue;
        scores.append({&candidate, scoreDecodedText(data, codec, candidate.preference)});
    }

    if (scores.isEmpty())
        return {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};

    int best = 0;
    int secondBest = -1;
    for (int i = 1; i < scores.size(); ++i) {
        if (scores.at(i).value > scores.at(best).value) {
            secondBest = best;
            best = i;
        } else if (secondBest < 0 || scores.at(i).value > scores.at(secondBest).value) {
            secondBest = i;
        }
    }

    const bool closeScores = secondBest >= 0 &&
                             scores.at(best).value - scores.at(secondBest).value <= 6;
    const bool shortSample = data.size() < 8;
    const Candidate *winner = scores.at(best).candidate;
    return {QString::fromLatin1(winner->label), QByteArray(winner->codecName), 0, false,
            shortSample || closeScores};
}

QString TextEncodingDetector::decode(const QByteArray &data, const Result &result) {
    if (result.binary || result.codecName.isEmpty())
        return QString();
    QTextCodec *codec = QTextCodec::codecForName(result.codecName);
    if (!codec)
        return QString::fromUtf8(data.mid(result.bomBytes));
    return codec->toUnicode(data.mid(result.bomBytes));
}

QByteArray TextEncodingDetector::safePrefix(const QByteArray &data, int maximumBytes) {
    if (maximumBytes <= 0)
        return QByteArray();
    if (data.size() <= maximumBytes)
        return data;

    const int minimumBytes = qMax(0, maximumBytes - 3);
    for (int length = maximumBytes; length >= minimumBytes; --length) {
        const QByteArray prefix = data.left(length);
        const Result result = detect(prefix);
        if (result.binary || result.codecName.isEmpty())
            continue;
        QTextCodec *codec = QTextCodec::codecForName(result.codecName);
        if (!codec)
            continue;
        const QByteArray payload = prefix.mid(result.bomBytes);
        const QString decoded = codec->toUnicode(payload);
        if (!decoded.contains(QChar::ReplacementCharacter) && codec->fromUnicode(decoded) == payload)
            return prefix;
    }
    return data.left(maximumBytes);
}
