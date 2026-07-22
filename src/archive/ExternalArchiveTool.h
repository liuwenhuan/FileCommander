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

    // Lists entries, invoking `cb` per entry. An empty password means "none".
    static Status list(const QString &archivePath, const QString &password,
                       const std::function<void(const Entry &)> &cb,
                       std::atomic<bool> *cancel = nullptr);

    // Extracts a single entry to `destPath` (for preview).
    static Status readEntry(const QString &archivePath, const QString &password,
                            const QString &entryPath, const QString &destPath,
                            std::atomic<bool> *cancel = nullptr);
};
