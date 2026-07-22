#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

// Reads a Type-2 AppImage as an archive. An AppImage is an ordinary ELF
// executable with a SquashFS filesystem appended after it; libarchive can't read
// squashfs, so this shells out to the system `unsquashfs` with an explicit byte
// offset (`-o <off>`) pointing at the appended filesystem. Preview only -- list
// entries and extract a single entry -- it never writes the archive and, crucially,
// NEVER executes the AppImage (no `--appimage-extract`/`--appimage-offset`, which
// would run the binary): it only reads the file's bytes and drives unsquashfs.
//
// Detection is by magic bytes, not suffix: many AppImages have no `.AppImage`
// extension. isAppImage() confirms the ELF magic and locates the squashfs (via the
// ELF section-header end, validated by the `hsqs` magic, with a scan fallback).
// Modelled on ExternalArchiveTool. If unsquashfs isn't installed, available() is
// false and list()/readEntry() return Unavailable.
class SquashfsReader {
public:
    enum class Status { Ok, Unavailable, Error };

    struct Entry {
        QString path;      // '/'-separated path within the filesystem (root-relative)
        qint64 size = 0;
        bool isDir = false;
        QDateTime modified;
    };

    // True iff `unsquashfs` is installed.
    static bool available();

    // True iff `path` is a Type-2 AppImage (ELF + appended squashfs). Cheap: reads
    // the ELF header and a few bytes at the computed offset; only files explicitly
    // tagged as AppImages trigger the whole-file magic scan.
    static bool isAppImage(const QString &path);

    // Byte offset of the appended SquashFS filesystem, or -1 if none was found.
    // Result is cached per (path, mtime, size) -- including the "not an AppImage"
    // negative -- so repeated recognition (e.g. QuickView selection changes) is cheap.
    static qint64 squashfsOffset(const QString &path);

    // SECURITY: entry names come from the (attacker-controllable) squashfs, so a
    // crafted image can embed '..' to escape the destination. Returns true only for
    // a relative path whose components are all non-empty and neither '.' nor '..'.
    static bool isSafeEntryPath(const QString &path);

    // SECURITY (defense in depth): true iff joining `relPath` onto `baseDir` stays
    // within `baseDir` after lexical normalisation.
    static bool isContained(const QString &baseDir, const QString &relPath);

    // Lists entries, invoking `cb` per entry. Entries with unsafe names (see
    // isSafeEntryPath) are dropped. `cancel` (optional) is polled to abort.
    static Status list(const QString &archivePath,
                       const std::function<void(const Entry &)> &cb,
                       std::atomic<bool> *cancel = nullptr);

    // Extracts a single entry to `destPath` (for preview). Rejects unsafe entry
    // names and symlinks (a symlink could point outside the archive).
    static Status readEntry(const QString &archivePath, const QString &entryPath,
                            const QString &destPath, std::atomic<bool> *cancel = nullptr);

    // Extracts the whole filesystem (or just `entries`, empty => everything) into
    // `destDir` in a SINGLE unsquashfs pass. Unsafe entry names are dropped;
    // unsquashfs (>= 4.5, CVE-2021-40153/41072 fixed) sanitises pathnames so
    // extraction cannot escape `destDir`.
    static Status extractTo(const QString &archivePath, const QStringList &entries,
                            const QString &destDir, std::atomic<bool> *cancel = nullptr);
};
