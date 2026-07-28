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
        bool binary = false;
        bool ambiguous = false;
    };

    static Result detect(const QByteArray &data);
};
