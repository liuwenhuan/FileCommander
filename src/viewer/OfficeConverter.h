#pragma once

#include <QString>
#include <QStringList>

// Wraps the `office_oxide` command-line tool
// (https://github.com/yfedoseev/office_oxide) as a subprocess to convert
// Office documents for read-only preview:
//   docx/doc/pptx/ppt -> HTML  (headings, bold/italic, paragraphs, tables)
//   xlsx/xls          -> TSV   (rendered as a grid by the caller)
//
// office_oxide is a Rust CLI (binary name `office-oxide`), the `office_oxide_cli`
// workspace member of that repo. NOTE: this expects our patched fork
// (codework/office_oxide-fork), which inlines embedded images into the HTML as
// base64 data: URIs -- upstream emits empty <img> placeholders. Build/install:
//   cargo install --path crates/office_oxide_cli --root ~/.local --force
// Its subcommands each take a single file path (no flags): text, markdown, html,
// info, ir. We use `html` for word/presentation files because its output is
// block-structured (proper <h1>/<p>/<strong>/<table>), which QTextBrowser renders
// faithfully -- `markdown` collapses list items and loses table blocks for lack
// of blank-line separators. Spreadsheets use `text`, which yields clean TSV that
// maps 1:1 to grid cells. Word docs keep the fork's inlined images; PowerPoint
// strips them (slide-positioned, don't line up with the flattened text preview).
class OfficeConverter {
public:
    enum class Kind { None, Document, Spreadsheet, Presentation };

    // True for .doc/.docx/.ppt/.pptx/.xls/.xlsx, case-insensitively.
    static bool isOfficeFile(const QString &path);

    // Document for word-processing/presentation files, Spreadsheet for
    // Excel files, None for anything else (including non-office files).
    // NOTE: PowerPoint files report Document here (the fallback text path);
    // convert() decides at run time whether a .pptx yields per-slide SVG
    // (Kind::Presentation) or falls back to the flattened text preview.
    static Kind kindFor(const QString &path);

    // True if the office_oxide CLI can be located; see resolveBinary().
    static bool isAvailable();

    // Resolves the office_oxide binary, or "" if it cannot be found.
    // Resolution order:
    //   1. $TTC_OFFICE_OXIDE, if set (explicit path override)
    //   2. PATH, trying candidate names "office_oxide", "office-oxide", "oxide"
    //   3. ~/.local/bin, then ~/.cargo/bin, same candidate names
    static QString resolveBinary();

    // Encryption state of a converted file. office_oxide decrypts in-process
    // (pure Rust, no Python): a password is handed to it via the
    // OFFICE_OXIDE_PASSWORD environment variable and it renders the plaintext
    // directly, so there is no separate decrypt step or temp file.
    enum class Encryption {
        None,          // not encrypted
        NeedsPassword, // encrypted; no (or blank) password was supplied
        WrongPassword, // a password was supplied but it is incorrect
        Unsupported,   // encrypted in a format we can't decrypt (legacy .xls/.ppt)
    };

    struct Result {
        bool ok = false;
        Kind kind = Kind::None;
        QString html;  // Document: HTML markup (render with QTextBrowser::setHtml)
        QString tsv;   // Spreadsheet: tab-separated cell text (one row per line)
        QStringList slideSvgs; // Presentation: one standalone SVG document per slide
        QString error; // human-readable message on failure
        Encryption encryption = Encryption::None;

        bool encrypted() const { return encryption != Encryption::None; }
    };

    // Runs the CLI synchronously (QProcess, ~30s timeout) and returns the
    // converted content. When `password` is non-empty it is passed to office_oxide
    // (which decrypts encrypted OOXML and legacy .doc in-process) via an
    // environment variable, never on the command line. Never throws: every failure
    // path sets ok=false, fills in `error`, and — for password-protected files —
    // sets `encryption` so the caller can prompt or report a wrong password.
    static Result convert(const QString &path, const QString &password = QString());

private:
    static Result convertDocument(const QString &binary, const QString &path,
                                  const QString &password);
    static Result convertSpreadsheet(const QString &binary, const QString &path,
                                     const QString &password);
    // PowerPoint (.pptx) → per-slide SVG (`office-oxide svg <file>`): stdout is a
    // JSON array of standalone SVG document strings. Legacy .ppt and any pptx the
    // renderer can't turn into slides print `[]`, leaving slideSvgs empty so the
    // caller can fall back to the text (html) preview.
    static Result convertPresentation(const QString &binary, const QString &path,
                                      const QString &password);
};
