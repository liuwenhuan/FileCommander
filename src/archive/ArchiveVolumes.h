#pragma once

#include <QString>
#include <QStringList>

// A split archive is one logical archive spread over `name.part01.rar`,
// `name.part02.rar`, ... Only the FIRST volume can be opened; the rest are
// continuations with no directory of their own.
//
// This matters more than it sounds, because the failure is silent. Measured
// against a real 9-volume RAR5 set, handing libarchive a member other than the
// first:
//
//   part05.rar -> ARCHIVE_EOF on the first read, no error at all: an archive
//                 that opens "successfully" and is empty.
//   part09.rar -> 105 entries, no error: an archive that looks perfectly normal
//                 and whose every entry is a tail fragment that cannot be
//                 extracted.
//
// So a caller cannot tell a volume member from a real archive by trying it.
// It has to be recognised by name, before opening, which is what this is for.
//
// libarchive cannot follow a volume chain in ANY of the three conventions
// (verified with bsdtar/libarchive 3.8.4 and 3.8.7), so a recognised member
// must be routed to the external 7z tool rather than opened in-process.
namespace fc {

// Whether `path` looks like one volume of a split set -- including the first.
// Name-only; does not touch the filesystem.
bool isVolumeMember(const QString &path);

// The path of the set's FIRST volume, or an empty string when `path` is not a
// volume member (or the first volume cannot be found on disk).
//
// Touches the filesystem on purpose: the first volume's extension is not
// derivable from a later member's. A RAR set's first volume is very often a
// self-extracting `.exe` -- the set this was written for is `Palworld.part01.exe`
// with `part02..part09.rar`, and computing `part01.rar` finds nothing at all.
// So the candidates are probed, and 7-Zip identifies the SFX stub by content
// rather than by extension (verified: renaming it to `.rar` lists identically).
//
// Returns `path` itself when `path` IS the first volume.
QString firstVolumeOf(const QString &path);

// Whether the set is a RAW BYTE SPLIT -- `name.7z.001`, `name.iso.002`, ...
// where the volumes concatenate back into the original file byte for byte.
// That distinction is what makes an in-process fallback possible: such a set
// can be streamed in order through one reader, with no external tool at all.
// A RAR set is NOT one -- every volume carries its own headers.
bool isRawSplit(const QString &firstVolume);

// Every volume of the set that exists on disk, in order, starting at
// `firstVolume`. Stops at the first gap, so a set missing volume 4 reports
// 1..3 rather than silently skipping to 5 and decoding garbage.
QStringList volumeChain(const QString &firstVolume);

} // namespace fc
