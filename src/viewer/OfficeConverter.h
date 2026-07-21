#pragma once

#include <QString>

// Wraps the `office_oxide` command-line tool
// (https://github.com/yfedoseev/office_oxide) as a subprocess to convert
// Office documents for read-only preview:
//   docx/doc/pptx/ppt -> HTML  (headings, bold/italic, paragraphs, tables)
//   xlsx/xls          -> TSV   (rendered as a grid by the caller)
//
// office_oxide is a Rust CLI (binary name `office-oxide`), the `office_oxide_cli`
// workspace member of that repo:
//   cargo install --git https://github.com/yfedoseev/office_oxide office_oxide_cli
// Its subcommands each take a single file path (no flags): text, markdown, html,
// info, ir. We use `html` for word/presentation files because its output is
// block-structured (proper <h1>/<p>/<strong>/<table>), which QTextBrowser renders
// faithfully -- `markdown` collapses list items and loses table blocks for lack
// of blank-line separators. Spreadsheets use `text`, which yields clean TSV that
// maps 1:1 to grid cells. office_oxide does NOT export embedded images (it emits
// empty <img> / broken ![](rIdN) placeholders), so those are stripped.
class OfficeConverter {
public:
    enum class Kind { None, Document, Spreadsheet };

    // True for .doc/.docx/.ppt/.pptx/.xls/.xlsx, case-insensitively.
    static bool isOfficeFile(const QString &path);

    // Document for word-processing/presentation files, Spreadsheet for
    // Excel files, None for anything else (including non-office files).
    static Kind kindFor(const QString &path);

    // True if the office_oxide CLI can be located; see resolveBinary().
    static bool isAvailable();

    // Resolves the office_oxide binary, or "" if it cannot be found.
    // Resolution order:
    //   1. $TTC_OFFICE_OXIDE, if set (explicit path override)
    //   2. PATH, trying candidate names "office_oxide", "office-oxide", "oxide"
    //   3. ~/.local/bin, then ~/.cargo/bin, same candidate names
    static QString resolveBinary();

    struct Result {
        bool ok = false;
        Kind kind = Kind::None;
        QString html;  // Document: HTML markup (render with QTextBrowser::setHtml)
        QString tsv;   // Spreadsheet: tab-separated cell text (one row per line)
        QString error; // human-readable message on failure
    };

    // Runs the CLI synchronously (QProcess, ~30s timeout) and returns the
    // converted content. Never throws: every failure path sets ok=false and
    // fills in `error` with a message suitable for display to the user.
    static Result convert(const QString &path);

private:
    static Result convertDocument(const QString &binary, const QString &path);
    static Result convertSpreadsheet(const QString &binary, const QString &path);
};
