#pragma once

#include <QByteArray>
#include <QString>

// Stateless text-encoding classifier used by QuickView's per-file Auto mode.
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
