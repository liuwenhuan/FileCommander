#pragma once

#include <QByteArray>
#include <QString>

// Stateless text-encoding classifier used by QuickView's per-file Auto mode.
// It deliberately has no QWidget dependency so its byte-level grammar and
// scoring behaviour can be tested without a running application.
class TextEncodingDetector {
public:
    struct Result {
        QString label;
        QByteArray codecName;
        int bomBytes = 0;
        bool binary = false;
        bool ambiguous = false;
    };

    static Result detect(const QByteArray &data);
    // Decodes a detected byte sequence after removing a validated BOM.
    static QString decode(const QByteArray &data, const Result &result);
    // Limits a preview without leaving an incomplete multi-byte sequence at its end.
    static QByteArray safePrefix(const QByteArray &data, int maximumBytes);
};
