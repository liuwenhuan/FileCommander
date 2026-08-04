#pragma once

#include <QString>

struct archive;
struct archive_entry;

// Entry names are the one place libarchive's C API and Qt disagree about what a
// byte string means, and getting it wrong is silent: the entry lists, and then
// cannot be opened.
//
// Two separate problems, both hit by the same archive -- a zip whose names were
// written in the local code page with general-purpose bit 11 clear (which is
// most zips made on a Chinese/Japanese/Korean Windows before UTF-8 names became
// the default, and still plenty made today):
//
//   * READING. QString::fromUtf8() on a non-UTF-8 name does not merely display
//     it wrongly, it DESTROYS it -- invalid bytes become U+FFFD and nothing
//     downstream can turn them back into something an extractor would match.
//   * WRITING. archive_entry_set_pathname() takes a multi-byte string in the
//     CURRENT LOCALE's encoding. On Windows that is the ANSI code page, not
//     UTF-8, so handing it QString::toUtf8() writes the file under a different
//     name than the caller then looks for. Measured: the extract "succeeded"
//     and QFile::exists() on the intended path was false.
//
// Everything here goes through wide characters on Windows, which is the one
// representation both sides agree on.
namespace fc {

// Call on every read handle AFTER the archive_read_support_format_*/filter_*
// calls and before opening. The option is dispatched to the modules already
// registered, so setting it first is silently ignored -- measured: names still
// came back decoded as Latin-1.
//
// Tells libarchive which code page to assume for entry names that carry no
// charset of their own. Only those: a zip entry flagged UTF-8, and every format
// that defines its names as UTF-8, is unaffected. Without it libarchive assumes
// CP437 for zip, which turns a GBK name into line-drawing characters.
void applyHeaderCharset(struct archive *a);

// The entry's name, decoded losslessly. Prefers libarchive's wide-character
// name (already converted using the charset above); falls back to the
// multi-byte name interpreted in the local 8-bit encoding, which is what the C
// API promises it to be -- never fromUtf8(), which is the lossy step.
QString entryPathname(struct archive_entry *entry);

// Sets the entry's name for archive_write_disk. The counterpart to the above:
// hands libarchive wide characters rather than bytes it would re-interpret.
void setEntryPathname(struct archive_entry *entry, const QString &path);

} // namespace fc
