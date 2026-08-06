#pragma once

class QTextCodec;

#include <QByteArray>
#include <QString>

// Stateless text-encoding classifier behind the per-file Auto mode of both the
// QuickView preview pane and the F4 editor, and the home of the encoding list
// their two toolbars offer.
// It deliberately has no QWidget dependency so its byte-level grammar and
// scoring behaviour can be tested without a running application.
class TextEncodingDetector {
public:
    enum class InputEnd { Complete, MayBeTruncated };

    struct Result {
        QString label;
        QByteArray codecName;
        int bomBytes = 0;
        bool binary = false;
        bool ambiguous = false;
        // Longest prefix of the original input that can be decoded completely.
        // It includes any BOM and equals the input size for a complete text result.
        int completePrefixBytes = 0;
        // True only when MayBeTruncated accepted one incomplete sequence at the
        // physical end of the probe; observed invalid bytes are never accepted.
        bool incompleteTail = false;
    };

    // One row of the encoding chooser, shared by the QuickView preview toolbar
    // (src/ui) and the F4 editor toolbar (src/viewer). It lives here rather than
    // in either of them because the two windows show the same files: a list that
    // diverged would let the preview and the editor disagree about what can even
    // be selected. The Auto row defers to detect() below.
    struct Selectable {
        const char *label;
        const char *codec; // null: Auto (index 0) / the system locale codec
    };
    // Addressed by INDEX -- QuickView persists the chooser's index rather than
    // its label -- so entries may be appended but never reordered or removed.
    static constexpr Selectable selectableEncodings[] = {
        {"Auto", nullptr},
        {"UTF-8", "UTF-8"},
        {"UTF-16", "UTF-16"},
        {"ISO-8859-1", "ISO-8859-1"},
        {"GB18030", "GB18030"},
        {"Big5", "Big5"},
        {"Shift-JIS", "Shift-JIS"},
        {"EUC-JP", "EUC-JP"},
        {"EUC-KR", "EUC-KR"},
        {"Windows-1252", "Windows-1252"},
        {"System", nullptr},
    };
    static constexpr int autoEncodingIndex = 0;
    static constexpr int selectableEncodingCount =
        int(sizeof(selectableEncodings) / sizeof(selectableEncodings[0]));

    // The codec for a chooser index, never null.
    //
    // Resolving one was four lines repeated at three call sites -- the preview
    // pane, the editor's load, the editor's save -- each doing the same two
    // fallbacks: a null codec entry means the system locale, and a locale Qt
    // cannot supply a codec for means UTF-8. Getting either fallback wrong
    // shows up as mojibake rather than as an error, which is exactly the kind
    // of rule that should not be copied.
    static QTextCodec *codecForSelectableIndex(int index);

    static Result detect(const QByteArray &data, InputEnd inputEnd = InputEnd::Complete);
    // The scoring sample is bounded; grammar validation still covers all input.
    static constexpr int legacyScoreSampleBytes() { return 64 * 1024; }
    // Decodes a detected byte sequence after removing a validated BOM.
    static QString decode(const QByteArray &data, const Result &result);
    // Limits a preview without leaving an incomplete multi-byte sequence at its end.
    // `result` is the single detection performed for the current file, so trimming
    // never re-scores adjacent multi-megabyte prefixes.
    static QByteArray safePrefix(const QByteArray &data, int maximumBytes, const Result &result);
};
