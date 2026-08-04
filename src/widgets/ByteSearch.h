#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

// Searching a file means searching its *bytes*. That sounds obvious until a
// needle is involved: what the user types is a QString, and turning it into
// bytes is a lossy, encoding-dependent step that has to be done with the same
// codec the file is being read as. Searching a UTF-8 buffer for a UTF-16
// encoded needle finds nothing at all -- not "fewer hits", nothing -- and the
// failure is silent, which is why this is a named module with tests rather than
// a call to QByteArray::indexOf at the call site.
//
// Deliberately free of widgets and QtGui: the editor, the F3 viewer and a hex
// pane can all share one search, and a pure-QByteArray core is the only version
// that can be tested without a window.
namespace ByteSearch {

enum class Mode {
    Text, // the user typed characters; encode them with `codecName`
    Hex,  // the user typed a byte sequence such as "4D 5A"
};

enum class Direction {
    Forward,
    Backward,
};

enum class CaseFolding {
    Exact,            // byte for byte
    AsciiInsensitive, // A-Z matches a-z; NOTHING else is folded (see FoldSupport)
};

// How much of "ignore case" a given encoding can actually honour.
//
// Case folding here is byte-level, so it can only ever reach the 52 ASCII
// letters. That is not a shortcut that could be lifted later: real folding
// (E9/C9, Cyrillic, Turkish dotless i) needs the decoded text, and decoding is
// exactly what a byte search does not do -- a large file, or one whose encoding
// is only guessed at, must not be decoded in its entirety to answer a search.
enum class FoldSupport {
    Full,           // ASCII letters fold; no other character is affected
    AsciiOnlyLossy, // folds, but this encoding also uses ASCII-letter byte
                    // values as trail bytes of multi-byte characters, so a
                    // match may start in the middle of a character
    Unsupported,    // the codec does not encode ASCII to identical single
                    // bytes (UTF-16/UTF-32), so byte folding is meaningless
};

// Probed against the codec itself rather than read from a hard-coded list, so a
// codec nobody thought of still gets a truthful answer. Result is cached.
FoldSupport foldSupport(const QByteArray &codecName);

// A needle that is ready to search with -- or a reason why it is not.
struct Needle {
    QByteArray bytes;
    // What will ACTUALLY be applied, which is not always what was requested:
    // asking for AsciiInsensitive under UTF-16 leaves this at Exact and fills
    // in `note`. Silently pretending otherwise would report "not found" for a
    // string that is in the file.
    CaseFolding folding = CaseFolding::Exact;
    bool valid = false;
    QString error; // user-visible reason it cannot be searched with
    QString note;  // user-visible caveat, even though the needle IS usable

    bool isEmpty() const { return bytes.isEmpty(); }
};

// `codecName` is a QTextCodec name ("UTF-8", "UTF-16", "GB18030", ...) and is
// ignored in Hex mode. An empty `text` yields an invalid needle with no error:
// an empty search box is not a mistake to complain about.
Needle compile(const QString &text, Mode mode, const QByteArray &codecName,
               CaseFolding requested);

// Lenient about spelling, strict about ambiguity. Accepts "4D 5A", "4d5a",
// "0x4D,0x5A", "\x4d\x5a", "4D-5A", "4D:5A" and newline-separated dumps.
//
// Rejects an odd number of nibbles, per group: "4D 5" is not obviously 0x4D
// 0x05 rather than 0x4D 0x50, and a search tool that guesses wrong reports a
// confident "not found" about the wrong bytes. One unseparated run is measured
// as a whole, so "4D5A" is fine and "4D5" is not.
Needle parseHex(const QString &text);

constexpr int kNotFound = -1;

// First match starting at or after `from`. With `wrap`, continues from 0.
int findForward(const QByteArray &haystack, const Needle &needle, int from, bool wrap);

// Last match starting strictly before `before` -- i.e. pass the offset of the
// current match to step off it. With `wrap`, continues from the end.
int findBackward(const QByteArray &haystack, const Needle &needle, int before, bool wrap);

// Convenience over the two above.
int find(const QByteArray &haystack, const Needle &needle, int from, Direction direction,
         bool wrap);

// Every offset at which the needle occurs, overlaps included ("aa" occurs twice
// in "aaa"): the counter beside a search box counts positions the user can be
// taken to, and a non-overlapping count would skip some of them.
int countMatches(const QByteArray &haystack, const Needle &needle);

// 1-based position of the match starting exactly at `offset` among all matches,
// or 0 when nothing starts there. Feeds the "3 / 12" readout.
int ordinalAt(const QByteArray &haystack, const Needle &needle, int offset);

} // namespace ByteSearch

Q_DECLARE_METATYPE(ByteSearch::Needle)
