#pragma once

#include <QSharedPointer>
#include <QString>
#include <QStringList>

#include <atomic>

#include "ArchiveNode.h"

// Thin facade over libarchive: build an in-memory tree of an archive's
// contents, and extract selected entries (or everything) to a directory.
// Supports whatever libarchive's "all formats" reader supports -- zip,
// tar, tar.gz, tar.bz2, tar.xz in practice.
class ArchiveHandler {
public:
    // Result of opening an archive for listing. Encrypted ZIP is readable once a
    // passphrase is supplied; encrypted 7z/rar is not (this libarchive has no
    // crypto backend for them), reported as EncryptedUnsupported.
    enum class Status { Ok, NeedPassword, WrongPassword, EncryptedUnsupported, Error };

    static bool isSupportedArchive(const QString &path);

    // Returns a null pointer on failure (see errorMessage).
    static QSharedPointer<ArchiveNode> buildTree(const QString &archivePath,
                                                  QString *errorMessage = nullptr);
    // Passphrase-aware variant. `status` (out) distinguishes an encrypted archive
    // that still needs a password, a wrong password, an unsupported encryption,
    // and plain errors. An empty passphrase just lists (encrypted ZIP lists its
    // names without one). Returns null unless *status == Ok.
    static QSharedPointer<ArchiveNode> buildTree(const QString &archivePath,
                                                  const QString &passphrase, Status *status,
                                                  QString *errorMessage = nullptr,
                                                  std::atomic<bool> *cancel = nullptr);

    // entryFullPaths empty => extract everything. A directory entry in
    // the list extracts that directory and everything under it.
    static bool extract(const QString &archivePath, const QStringList &entryFullPaths,
                         const QString &destDir, QString *errorMessage = nullptr);
    // Passphrase-aware variant (for extracting from an encrypted archive).
    static bool extract(const QString &archivePath, const QStringList &entryFullPaths,
                         const QString &destDir, const QString &passphrase,
                         QString *errorMessage);

    // Outcome of a Bandizip-style "smart" whole-archive extraction.
    struct SmartResult {
        bool ok = false;
        QString finalDir;          // directory the contents actually landed in
        QString nestedArchivePath; // set iff the result is a single inner archive
    };

    // Extracts the whole archive into `baseDestDir`, applying Bandizip's layout
    // rules (see ArchiveLayout): a single top-level folder is extracted as-is
    // (no double nesting); multiple top-level items are wrapped in a folder named
    // after the archive. A colliding target folder is disambiguated with a " (n)"
    // suffix so an extraction never clobbers existing files. If the extracted
    // result is itself a single archive, its path is reported in
    // `nestedArchivePath` so the caller may offer recursive extraction.
    static SmartResult smartExtract(const QString &archivePath, const QString &baseDestDir,
                                    QString *errorMessage = nullptr);

    // format: one of "zip", "tar", "tar.gz", "tar.bz2", "tar.xz".
    // Each entry in sourcePaths (file or directory) is added at the
    // archive root under its own basename.
    // Requested passphrase / header-encryption / compression are forwarded to the
    // underlying libarchive writer when the format supports them; otherwise they
    // are ignored silently.
    static bool create(const QString &archivePath, const QStringList &sourcePaths,
                        const QString &format, QString *errorMessage = nullptr);

    static bool create(const QString &archivePath, const QStringList &sourcePaths,
                        const QString &format, const QString &passphrase,
                        bool encryptHeaders, int compressionLevel,
                        QString *errorMessage = nullptr);
};
