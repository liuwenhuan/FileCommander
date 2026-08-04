#pragma once

#include <QByteArray>
#include <QString>

// Decides whether F4 should open a file in the text editor or the hex editor.
//
// The answer comes from the bytes, never from the extension: a .dat holding
// UTF-8 config is text, and a .txt holding a JPEG somebody renamed is not.
// The encoding grammar itself is not re-implemented here -- this asks
// TextEncodingDetector (the same classifier QuickView's Auto mode uses) and
// then applies one editor-specific policy layer on top of its verdict, so the
// preview pane and the editor can never disagree about what a file is.
namespace fc {

// Why a file was routed where it was. Deliberately an enum rather than a
// sentence: the caller owns any user-facing wording and its translation, so
// this header stays free of UI strings.
enum class SniffReason {
    EmptyFile,               // nothing to judge; an empty file is editable text
    DecodedAsText,           // an encoding was named and its decode looks like prose
    SingleByteFallback,      // no encoding named, but the bytes are plain 8-bit text
    DetectorReportedBinary,  // NUL bytes or a control-character flood
    NoEncodingDecodes,       // every candidate grammar rejected the sample
    AmbiguousWithNulBytes,   // the detector admitted a coin-flip over NUL-bearing bytes
    ControlCharacterDensity, // an encoding decoded, but into control characters
};

struct SniffResult {
    bool hex = false;
    SniffReason reason = SniffReason::DecodedAsText;
    // TextEncodingDetector's label ("UTF-8", "UTF-16LE", "GB18030", ...) and a
    // name QTextCodec::codecForName() accepts. Both are empty when hex is true.
    QString encodingLabel;
    QByteArray codecName;
};

// How many leading bytes are worth reading before asking. Reading more rarely
// changes the answer and always costs a round trip on a network provider.
int sniffSampleBytes();

// `sample` is the first sniffSampleBytes() of the file (fewer if it is
// shorter); it is treated as possibly truncated mid-character. `path` is
// accepted because every caller already has it and it belongs in diagnostics,
// but it is deliberately not consulted: the verdict is content-only.
SniffResult sniff(const QByteArray &sample, const QString &path);
bool shouldEditAsHex(const QByteArray &sample, const QString &path);

} // namespace fc
