#pragma once

#include <QString>
#include <QStringList>

// Pure, testable structure analysis for "smart" archive extraction / browsing,
// modelled on Bandizip's rules. Given the list of an archive's entry paths (as
// they appear inside the archive, POSIX-style, directory entries optionally
// ending in '/') plus the archive's base name, it decides how the contents
// should be laid out when extracted or browsed.
//
// The three cases:
//   * Single top-level folder -- every entry lives under one common top-level
//     directory. That directory is "stripped": the browse root / extraction
//     shows its *contents* directly instead of double-nesting.
//   * Single top-level file -- exactly one file at the root and nothing else.
//     It is extracted directly (no wrapper folder, nothing to strip).
//   * Multiple top-level items -- 2+ files/folders at the root. They are wrapped
//     under one folder named after the archive (extraction) / shown as-is under
//     a synthesized root (browse).
//
// Independently it reports whether the (stripped) result is effectively a single
// archive file, so the caller may offer recursive extraction. Detection only --
// the recursive loop is the caller's concern.
namespace ArchiveLayout {

struct Result {
    // Strip a single common top-level directory (its contents become the root).
    bool stripSingleRoot = false;
    // The stripped directory's name (no trailing slash). Valid iff stripSingleRoot.
    QString strippedPrefix;
    // Wrap the (multiple) top-level items under a folder named after the archive.
    bool wrapInArchiveNamedFolder = false;
    // The sole remaining top-level entry is itself an archive file.
    bool resultIsSingleArchive = false;
    // Name of that inner archive (basename). Valid iff resultIsSingleArchive.
    QString innerArchiveName;
};

// Pure analysis. `archiveBaseName` is the archive's file name (used only for
// context by callers naming a wrapper folder; the analysis does not mutate it).
Result analyze(const QStringList &entryPaths, const QString &archiveBaseName);

// Suffix check: does `name` look like an archive file? Shared with
// ArchiveProvider::isArchivePath.
bool hasArchiveSuffix(const QString &name);

} // namespace ArchiveLayout
