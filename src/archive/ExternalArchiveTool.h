#pragma once

#include <QDateTime>
#include <QString>

#include <atomic>
#include <functional>

// Last-resort preview fallback: when neither the in-process SevenZipReader nor
// libarchive can open an archive (typically encrypted RAR, or a coder the
// bundled decoders don't implement), shell out to a system-installed `7z` /
// `unrar` if one is present. Preview only -- list entries and read a single
// entry -- never writes or creates archives. If no suitable tool is installed,
// returns Unavailable and the caller keeps its existing "unsupported" behaviour.
class ExternalArchiveTool {
public:
    enum class Status { Ok, NeedPassword, WrongPassword, Unavailable, Error };

    struct Entry {
        QString path;      // '/'-separated path within the archive
        qint64 size = 0;
        bool isDir = false;
        QDateTime modified;
    };

    // True iff a tool that can read this archive's format is installed.
    static bool available(const QString &archivePath);

    // True iff this archive should be read via the external tool INSTEAD of
    // libarchive, rather than only as a fallback after libarchive fails.
    //
    // The case that matters is a UDF disc image (most modern install ISOs are
    // UDF with an ISO9660 "bridge" stub for compatibility). libarchive opens
    // such a file successfully but only sees the tiny bridge -- typically a
    // couple of placeholder entries -- so no failure ever occurs to trigger the
    // normal fallback and the user is shown an empty-looking image. 7z reads the
    // real UDF tree, so route to it up front. Detected from the volume
    // recognition sequence (NSR02/NSR03), not the suffix, so a plain ISO9660
    // image keeps using in-process libarchive.
    static bool preferExternal(const QString &archivePath);

    // Lists entries, invoking `cb` per entry. An empty password means "none".
    static Status list(const QString &archivePath, const QString &password,
                       const std::function<void(const Entry &)> &cb,
                       std::atomic<bool> *cancel = nullptr);

    // Extracts a single entry to `destPath` (for preview).
    //
    // `expectedSize` (>= 0, from a prior list()) is the entry's uncompressed
    // size. It makes a non-zero tool exit safe to tolerate: 7z reports one for
    // non-fatal complaints -- a UDF bridge image exits 2 over its malformed
    // ISO9660 stub -- while still extracting correctly, so the byte count, not
    // the exit code, decides whether the data is good. Pass -1 when the size
    // isn't known; the result is then only checked for being non-empty.
    static Status readEntry(const QString &archivePath, const QString &password,
                            const QString &entryPath, const QString &destPath,
                            std::atomic<bool> *cancel = nullptr, qint64 expectedSize = -1);
};
