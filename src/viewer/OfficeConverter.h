#pragma once

#include <QString>

// Wraps the `office_oxide` command-line tool
// (https://github.com/yfedoseev/office_oxide, docs at https://office.oxide.fyi)
// as a subprocess to convert Office documents for read-only preview:
//   docx/doc/pptx/ppt -> Markdown text + images extracted into a temp dir
//   xlsx/xls          -> CSV text (rendered as a table by the caller)
//
// office_oxide is a Rust CLI installed via `cargo install office_oxide_cli`
// (binary name `office-oxide`). It is NOT installed in the environment this
// module was written in, so the exact invocations below are built
// defensively from the *documented* CLI surface rather than tested against
// a real binary:
//   office-oxide text <file>       -- plain text
//   office-oxide markdown <file>   -- markdown
//   office-oxide html <file>       -- html
//   office-oxide info <file>       -- metadata
//   office-oxide ir <file>         -- JSON IR dump
//
// Two things this module needs are NOT present in that documented surface:
// a dedicated image-extraction option for `markdown` (the IR only carries
// image alt text, not bytes, over the CLI), and a `csv` subcommand (the
// underlying library has RFC-4180 CSV support internally, but the CLI does
// not expose it in the README/source as of this writing). convert() tries
// the most plausible flag/subcommand first and transparently falls back to
// a safe, documented invocation if the CLI rejects it as unrecognized. See
// the ASSUMPTION comments in OfficeConverter.cpp -- re-check them against
// `office-oxide --help` once the binary is actually installed.
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
        QString markdown; // Document: markdown text; image links resolve inside workDir
        QString csv;      // Spreadsheet: CSV text
        QString workDir;  // temp dir holding extracted images; empty if none. Caller owns cleanup.
        QString error;    // human-readable message on failure
    };

    // Runs the CLI synchronously (QProcess, ~30s timeout) and returns the
    // converted content. Never throws: every failure path sets ok=false and
    // fills in `error` with a message suitable for display to the user.
    static Result convert(const QString &path);

private:
    static Result convertDocument(const QString &binary, const QString &path);
    static Result convertSpreadsheet(const QString &binary, const QString &path);
};
