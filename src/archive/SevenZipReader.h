#pragma once

#include <QDateTime>
#include <QString>

#include <atomic>
#include <functional>

// Minimal, self-contained 7z reader for *preview*: lists entries and extracts a
// single entry, including AES-256 encrypted and solid archives -- the cases the
// system libarchive can't decode. It wraps the vendored public-domain LZMA SDK
// (LZMA/LZMA2/PPMd/BCJ/Delta decode) plus a small AES-256/SHA-256 key-derivation
// glue (using libnettle), so it reimplements none of the hard codecs/crypto and
// only drives the 7z container format. Not a general tool: no writing, no repair.
class SevenZipReader {
public:
    enum class Status { Ok, NeedPassword, WrongPassword, Unsupported, Error };

    struct Entry {
        QString path;      // '/'-separated path within the archive
        qint64 size = 0;
        bool isDir = false;
        QDateTime modified;
    };

    // Lists entries, invoking `cb` per entry. An empty password means "none"; if
    // the archive is encrypted and no (or a wrong) password is given, returns
    // NeedPassword / WrongPassword. `cancel` (optional) is polled to abort.
    static Status list(const QString &archivePath, const QString &password,
                       const std::function<void(const Entry &)> &cb,
                       std::atomic<bool> *cancel = nullptr);

    // Decrypts + decompresses a single entry to `destPath` (for preview).
    static Status readEntry(const QString &archivePath, const QString &password,
                            const QString &entryPath, const QString &destPath,
                            std::atomic<bool> *cancel = nullptr);
};
