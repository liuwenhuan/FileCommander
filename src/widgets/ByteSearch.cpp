#include "ByteSearch.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QTextCodec>
#include <QVector>

#include <cstring>
#include <memory>

namespace {

const char *const kContext = "ByteSearch";

QString tr(const char *text) {
    return QCoreApplication::translate(kContext, text);
}

inline bool isHexDigit(QChar c) {
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

inline bool isSeparator(QChar c) {
    switch (c.unicode()) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case ',':
    case ';':
    case ':':
    case '-':
    case '_':
    case '|':
    case '.':
        return true;
    default:
        return false;
    }
}

inline uchar foldByte(uchar b) {
    return (b >= 'A' && b <= 'Z') ? static_cast<uchar>(b - 'A' + 'a') : b;
}

// One scanner for both directions and both folding modes. Exact comparison goes
// through memcmp so the common case stays cheap; the folded one has to walk the
// bytes because the table only touches 52 of the 256 values.
bool matchesAt(const char *hay, const char *needle, int needleSize, int pos, bool fold) {
    if (!fold)
        return std::memcmp(hay + pos, needle, static_cast<size_t>(needleSize)) == 0;
    for (int i = 0; i < needleSize; ++i) {
        if (foldByte(static_cast<uchar>(hay[pos + i])) !=
            foldByte(static_cast<uchar>(needle[i])))
            return false;
    }
    return true;
}

int scanForward(const QByteArray &haystack, const QByteArray &needle, int from, bool fold) {
    const int last = haystack.size() - needle.size();
    if (last < 0)
        return ByteSearch::kNotFound;
    if (from < 0)
        from = 0;
    const char *hay = haystack.constData();
    const char *pat = needle.constData();
    const int size = needle.size();
    for (int pos = from; pos <= last; ++pos) {
        if (matchesAt(hay, pat, size, pos, fold))
            return pos;
    }
    return ByteSearch::kNotFound;
}

int scanBackward(const QByteArray &haystack, const QByteArray &needle, int startAtMost,
                 bool fold) {
    const int last = haystack.size() - needle.size();
    if (last < 0)
        return ByteSearch::kNotFound;
    int pos = qMin(startAtMost, last);
    const char *hay = haystack.constData();
    const char *pat = needle.constData();
    const int size = needle.size();
    for (; pos >= 0; --pos) {
        if (matchesAt(hay, pat, size, pos, fold))
            return pos;
    }
    return ByteSearch::kNotFound;
}

bool usable(const ByteSearch::Needle &needle) {
    return needle.valid && !needle.bytes.isEmpty();
}

bool foldingActive(const ByteSearch::Needle &needle) {
    return needle.folding == ByteSearch::CaseFolding::AsciiInsensitive;
}

// Does this codec put every ASCII character at its own byte value, alone? If
// not (UTF-16, UTF-32, EBCDIC), a byte-level fold table is not describing case
// at all and must not be applied.
bool asciiTransparent(QTextCodec *codec) {
    std::unique_ptr<QTextEncoder> encoder(codec->makeEncoder(QTextCodec::IgnoreHeader));
    if (!encoder)
        return false;
    for (ushort c = 0x20; c < 0x7f; ++c) {
        const QByteArray encoded = encoder->fromUnicode(QString(QChar(c)));
        if (encoded.size() != 1 || static_cast<uchar>(encoded.at(0)) != c)
            return false;
    }
    return true;
}

// Does this codec reuse ASCII-letter byte values as the trail byte of a
// multi-byte character? Shift-JIS, GB18030 and Big5 all do. Probed by decoding
// candidate pairs rather than by consulting a list of codec names, so an
// encoding nobody enumerated here still gets a truthful answer.
bool reusesAsciiLetterBytes(QTextCodec *codec) {
    for (ushort lead = 0x80; lead <= 0xff; ++lead) {
        for (ushort trail = 'A'; trail <= 'z'; ++trail) {
            if (trail > 'Z' && trail < 'a')
                continue;
            const char pair[2] = {static_cast<char>(lead), static_cast<char>(trail)};
            QTextCodec::ConverterState state;
            const QString decoded = codec->toUnicode(pair, 2, &state);
            if (state.invalidChars == 0 && state.remainingChars == 0 && decoded.size() == 1 &&
                decoded.at(0) != QChar(0xfffd))
                return true;
        }
    }
    return false;
}

} // namespace

namespace ByteSearch {

FoldSupport foldSupport(const QByteArray &codecName) {
    static QMutex mutex;
    static QHash<QByteArray, FoldSupport> cache;

    QMutexLocker locker(&mutex);
    const auto cached = cache.constFind(codecName);
    if (cached != cache.constEnd())
        return cached.value();

    FoldSupport result = FoldSupport::Unsupported;
    if (QTextCodec *codec = QTextCodec::codecForName(codecName)) {
        if (asciiTransparent(codec)) {
            result = reusesAsciiLetterBytes(codec) ? FoldSupport::AsciiOnlyLossy
                                                   : FoldSupport::Full;
        }
    }
    cache.insert(codecName, result);
    return result;
}

Needle parseHex(const QString &text) {
    Needle needle;
    QVector<QString> groups;
    QString current;
    const int size = text.size();
    int i = 0;
    while (i < size) {
        const QChar c = text.at(i);
        if (isSeparator(c)) {
            if (!current.isEmpty()) {
                groups.append(current);
                current.clear();
            }
            ++i;
            continue;
        }
        // Per-byte spellings people paste from debuggers and hex dumps. A
        // prefix also ends whatever group precedes it, because "\x4d\x5a" is
        // written without separators and each escape starts a byte of its own.
        // Recognising "0x" this way costs nothing: 'x' is not a hex digit, so
        // the alternative reading is an error message.
        if (c == QLatin1Char('$') || c == QLatin1Char('%')) {
            if (!current.isEmpty()) {
                groups.append(current);
                current.clear();
            }
            ++i;
            continue;
        }
        if ((c == QLatin1Char('0') || c == QLatin1Char('\\')) && i + 1 < size) {
            const QChar next = text.at(i + 1);
            if (next == QLatin1Char('x') || next == QLatin1Char('X')) {
                if (!current.isEmpty()) {
                    groups.append(current);
                    current.clear();
                }
                i += 2;
                continue;
            }
        }
        if (!isHexDigit(c)) {
            needle.error = QCoreApplication::translate(
                               kContext, "'%1' is not a hex digit — use 0-9 and A-F.")
                               .arg(c);
            return needle;
        }
        current.append(c);
        ++i;
    }
    if (!current.isEmpty())
        groups.append(current);

    if (groups.isEmpty()) {
        needle.error = tr("Enter hex digits, for example 4D 5A.");
        return needle;
    }

    QByteArray bytes;
    for (const QString &group : groups) {
        if (group.size() % 2 != 0) {
            // Not a guess we are entitled to make: "4D 5" could be 0x4D 0x05 or
            // 0x4D 0x50, and picking one produces a confident "not found" about
            // bytes the user never asked for.
            needle.error =
                QCoreApplication::translate(
                    kContext, "'%1' has an odd number of hex digits — a byte needs two.")
                    .arg(group);
            return needle;
        }
        for (int pos = 0; pos < group.size(); pos += 2)
            bytes.append(static_cast<char>(group.midRef(pos, 2).toUShort(nullptr, 16)));
    }

    needle.bytes = bytes;
    needle.folding = CaseFolding::Exact; // bytes are bytes; there is no case
    needle.valid = true;
    return needle;
}

Needle compile(const QString &text, Mode mode, const QByteArray &codecName,
               CaseFolding requested) {
    Needle needle;
    if (text.isEmpty())
        return needle; // an empty box is not a mistake worth a message
    if (mode == Mode::Hex)
        return parseHex(text);

    QTextCodec *codec = QTextCodec::codecForName(codecName);
    if (!codec) {
        needle.error = QCoreApplication::translate(kContext, "Unknown encoding: %1")
                           .arg(QString::fromLatin1(codecName));
        return needle;
    }

    // IgnoreHeader matters more than it looks: the plain "UTF-16" codec writes a
    // byte-order mark ahead of the text, and a needle carrying FF FE matches
    // nothing beyond the very start of a file.
    std::unique_ptr<QTextEncoder> encoder(codec->makeEncoder(QTextCodec::IgnoreHeader));
    if (!encoder) {
        needle.error = QCoreApplication::translate(kContext, "Unknown encoding: %1")
                           .arg(QString::fromLatin1(codecName));
        return needle;
    }
    const QByteArray bytes = encoder->fromUnicode(text);
    if (encoder->hasFailure()) {
        // The codec substituted '?' for something it cannot write. Searching for
        // the substitute would answer a question nobody asked.
        needle.error = QCoreApplication::translate(
                           kContext, "'%1' cannot be written in %2 — searching it is impossible.")
                           .arg(text, QString::fromLatin1(codec->name()));
        return needle;
    }

    needle.bytes = bytes;
    needle.valid = true;
    needle.folding = CaseFolding::Exact;

    if (requested == CaseFolding::AsciiInsensitive) {
        const QString label = QString::fromLatin1(codec->name());
        switch (foldSupport(codecName)) {
        case FoldSupport::Full:
            needle.folding = CaseFolding::AsciiInsensitive;
            break;
        case FoldSupport::AsciiOnlyLossy:
            needle.folding = CaseFolding::AsciiInsensitive;
            needle.note = QCoreApplication::translate(
                              kContext,
                              "Ignoring case folds ASCII letters only, and %1 also uses those "
                              "byte values inside multi-byte characters — a match may not start "
                              "on a character boundary.")
                              .arg(label);
            break;
        case FoldSupport::Unsupported:
            needle.note = QCoreApplication::translate(
                              kContext, "Ignoring case is not possible in %1 — searching exactly.")
                              .arg(label);
            break;
        }
    }
    return needle;
}

int findForward(const QByteArray &haystack, const Needle &needle, int from, bool wrap) {
    if (!usable(needle))
        return kNotFound;
    const bool fold = foldingActive(needle);
    const int hit = scanForward(haystack, needle.bytes, from, fold);
    if (hit != kNotFound || !wrap)
        return hit;
    // A full rescan rather than a bounded one: the tail found nothing, so every
    // remaining match necessarily starts before `from`, and the first hit of a
    // scan from zero is the one the user expects to land on.
    return scanForward(haystack, needle.bytes, 0, fold);
}

int findBackward(const QByteArray &haystack, const Needle &needle, int before, bool wrap) {
    if (!usable(needle))
        return kNotFound;
    const bool fold = foldingActive(needle);
    if (before > 0) {
        const int hit = scanBackward(haystack, needle.bytes, before - 1, fold);
        if (hit != kNotFound)
            return hit;
    }
    if (!wrap)
        return kNotFound;
    return scanBackward(haystack, needle.bytes, haystack.size(), fold);
}

int find(const QByteArray &haystack, const Needle &needle, int from, Direction direction,
         bool wrap) {
    return direction == Direction::Forward ? findForward(haystack, needle, from, wrap)
                                           : findBackward(haystack, needle, from, wrap);
}

int countMatches(const QByteArray &haystack, const Needle &needle) {
    if (!usable(needle))
        return 0;
    const bool fold = foldingActive(needle);
    int count = 0;
    int pos = scanForward(haystack, needle.bytes, 0, fold);
    while (pos != kNotFound) {
        ++count;
        pos = scanForward(haystack, needle.bytes, pos + 1, fold);
    }
    return count;
}

int ordinalAt(const QByteArray &haystack, const Needle &needle, int offset) {
    if (!usable(needle) || offset < 0 || offset > haystack.size() - needle.bytes.size())
        return 0;
    const bool fold = foldingActive(needle);
    if (!matchesAt(haystack.constData(), needle.bytes.constData(), needle.bytes.size(), offset,
                   fold))
        return 0;
    int ordinal = 1;
    int pos = scanForward(haystack, needle.bytes, 0, fold);
    while (pos != kNotFound && pos < offset) {
        ++ordinal;
        pos = scanForward(haystack, needle.bytes, pos + 1, fold);
    }
    return ordinal;
}

} // namespace ByteSearch
