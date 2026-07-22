#include "ArchiveHandler.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFileInfo>
#include <QVector>

#include "ArchiveLayout.h"
#include "ExternalArchiveTool.h"
#include "SevenZipReader.h"
#include "SquashfsReader.h"

namespace {

QString lastArchiveError(struct archive *a) {
    const char *msg = archive_error_string(a);
    return msg ? QString::fromUtf8(msg) : QString();
}

// .7z gets its own in-process reader (SevenZipReader) rather than libarchive:
// it decodes AES-256 encrypted archives (which this libarchive cannot) and
// lists solid archives from the header index without a full decompress.
bool isSevenZip(const QString &path) {
    return path.trimmed().toLower().endsWith(QStringLiteral(".7z"));
}

// An AppImage (ELF + appended squashfs) is browsable only when unsquashfs is
// installed. Detection is by magic bytes -- most AppImages carry no suffix.
bool isAppImage(const QString &path) {
    return SquashfsReader::available() && SquashfsReader::isAppImage(path);
}

// Lists a .7z-style tree for an AppImage via unsquashfs, mirroring extractSevenZip.
bool extractAppImage(const QString &archivePath, const QStringList &sel, const QString &destDir,
                     QString *errorMessage) {
    QVector<SquashfsReader::Entry> files;
    const SquashfsReader::Status ls = SquashfsReader::list(
        archivePath, [&](const SquashfsReader::Entry &e) {
            if (!e.isDir)
                files.append(e);
        });
    if (ls != SquashfsReader::Status::Ok) {
        if (errorMessage)
            *errorMessage = QStringLiteral("AppImage list: %1").arg(int(ls));
        return false;
    }
    QDir().mkpath(destDir);
    bool ok = true;
    for (const SquashfsReader::Entry &e : files) {
        bool want = sel.isEmpty();
        for (const QString &s : sel) {
            if (e.path == s || e.path.startsWith(s + QLatin1Char('/'))) {
                want = true;
                break;
            }
        }
        if (!want)
            continue;
        const QString destPath = QDir(destDir).filePath(e.path);
        const SquashfsReader::Status rs =
            SquashfsReader::readEntry(archivePath, e.path, destPath);
        if (rs != SquashfsReader::Status::Ok) {
            ok = false;
            if (errorMessage && errorMessage->isEmpty())
                *errorMessage = QStringLiteral("AppImage extract '%1': %2").arg(e.path).arg(int(rs));
        }
    }
    return ok;
}

ArchiveHandler::Status sevenZipToStatus(SevenZipReader::Status s) {
    switch (s) {
    case SevenZipReader::Status::Ok:            return ArchiveHandler::Status::Ok;
    case SevenZipReader::Status::NeedPassword:  return ArchiveHandler::Status::NeedPassword;
    case SevenZipReader::Status::WrongPassword: return ArchiveHandler::Status::WrongPassword;
    case SevenZipReader::Status::Unsupported:   return ArchiveHandler::Status::EncryptedUnsupported;
    case SevenZipReader::Status::Error:         return ArchiveHandler::Status::Error;
    }
    return ArchiveHandler::Status::Error;
}

// Extracts selected entries (empty selection => everything) from a .7z via the
// in-process reader, preserving the archive's directory layout under destDir.
// `*handled` is set false only when the reader can't decode this container, so
// the caller can fall back to libarchive. Each readEntry re-decodes the entry's
// solid block; fine for preview-scale use.
bool extractSevenZip(const QString &archivePath, const QStringList &sel, const QString &destDir,
                     const QString &passphrase, QString *errorMessage, bool *handled) {
    QVector<SevenZipReader::Entry> files;
    const SevenZipReader::Status ls = SevenZipReader::list(
        archivePath, passphrase,
        [&](const SevenZipReader::Entry &e) {
            if (!e.isDir)
                files.append(e);
        });
    if (ls == SevenZipReader::Status::Unsupported) {
        *handled = false;
        return false;
    }
    *handled = true;
    if (ls != SevenZipReader::Status::Ok) {
        if (errorMessage)
            *errorMessage = QStringLiteral("7z: %1").arg(int(ls));
        return false;
    }

    QDir().mkpath(destDir);
    bool ok = true;
    for (const SevenZipReader::Entry &e : files) {
        bool want = sel.isEmpty();
        for (const QString &s : sel) {
            if (e.path == s || e.path.startsWith(s + QLatin1Char('/'))) {
                want = true;
                break;
            }
        }
        if (!want)
            continue;
        const QString destPath = QDir(destDir).filePath(e.path);
        QDir().mkpath(QFileInfo(destPath).absolutePath());
        const SevenZipReader::Status rs =
            SevenZipReader::readEntry(archivePath, passphrase, e.path, destPath);
        if (rs != SevenZipReader::Status::Ok) {
            ok = false;
            if (errorMessage && errorMessage->isEmpty())
                *errorMessage = QStringLiteral("7z extract '%1': %2").arg(e.path).arg(int(rs));
        }
    }
    return ok;
}

// Same shape as extractSevenZip but via the external CLI tool -- the extract-side
// fallback for archives libarchive can't decode (e.g. encrypted RAR).
bool extractExternal(const QString &archivePath, const QStringList &sel, const QString &destDir,
                     const QString &passphrase, QString *errorMessage) {
    QVector<ExternalArchiveTool::Entry> files;
    const ExternalArchiveTool::Status ls = ExternalArchiveTool::list(
        archivePath, passphrase,
        [&](const ExternalArchiveTool::Entry &e) {
            if (!e.isDir)
                files.append(e);
        });
    if (ls != ExternalArchiveTool::Status::Ok) {
        if (errorMessage && errorMessage->isEmpty())
            *errorMessage = QStringLiteral("external list: %1").arg(int(ls));
        return false;
    }
    QDir().mkpath(destDir);
    bool ok = true;
    for (const ExternalArchiveTool::Entry &e : files) {
        bool want = sel.isEmpty();
        for (const QString &s : sel) {
            if (e.path == s || e.path.startsWith(s + QLatin1Char('/'))) {
                want = true;
                break;
            }
        }
        if (!want)
            continue;
        const QString destPath = QDir(destDir).filePath(e.path);
        QDir().mkpath(QFileInfo(destPath).absolutePath());
        const ExternalArchiveTool::Status rs =
            ExternalArchiveTool::readEntry(archivePath, passphrase, e.path, destPath);
        if (rs != ExternalArchiveTool::Status::Ok) {
            ok = false;
            if (errorMessage && errorMessage->isEmpty())
                *errorMessage = QStringLiteral("external extract '%1': %2").arg(e.path).arg(int(rs));
        }
    }
    return ok;
}

QSharedPointer<ArchiveNode> ensurePath(QSharedPointer<ArchiveNode> &root, const QStringList &parts,
                                        bool leafIsDir, qint64 size, const QDateTime &modified) {
    QSharedPointer<ArchiveNode> current = root;
    QString accumulated;
    for (int i = 0; i < parts.size(); ++i) {
        const QString &part = parts.at(i);
        if (part.isEmpty())
            continue;
        accumulated = accumulated.isEmpty() ? part : accumulated + QLatin1Char('/') + part;
        QSharedPointer<ArchiveNode> child = current->findChild(part);
        const bool isLast = (i == parts.size() - 1);
        if (!child) {
            child = QSharedPointer<ArchiveNode>::create();
            child->name = part;
            child->fullPath = accumulated;
            child->isDir = isLast ? leafIsDir : true;
            child->parent = current.data();
            current->children.append(child);
        }
        if (isLast) {
            child->size = size;
            child->modified = modified;
        }
        current = child;
    }
    return current;
}

int copyData(struct archive *ar, struct archive *aw) {
    const void *buff;
    size_t size;
    la_int64_t offset;
    for (;;) {
        int r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF)
            return ARCHIVE_OK;
        if (r < ARCHIVE_OK)
            return r;
        r = archive_write_data_block(aw, buff, size, offset);
        if (r < ARCHIVE_OK)
            return r;
    }
}

bool addEntryRecursive(struct archive *a, const QString &fsPath, const QString &archivePath,
                        QString *errorMessage) {
    QFileInfo info(fsPath);
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, archivePath.toUtf8().constData());
    archive_entry_set_mtime(entry, info.lastModified().toSecsSinceEpoch(), 0);
    archive_entry_set_perm(entry, info.isDir() ? 0755 : 0644);

    if (info.isDir()) {
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_size(entry, 0);
        int wr = archive_write_header(a, entry);
        archive_entry_free(entry);
        if (wr < ARCHIVE_OK) {
            if (errorMessage)
                *errorMessage = QString::fromUtf8(archive_error_string(a));
            return false;
        }

        const QFileInfoList children =
            QDir(fsPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &child : children) {
            const QString childArchivePath = archivePath + QLatin1Char('/') + child.fileName();
            if (!addEntryRecursive(a, child.absoluteFilePath(), childArchivePath, errorMessage))
                return false;
        }
        return true;
    }

    QFile file(fsPath);
    if (!file.open(QIODevice::ReadOnly)) {
        archive_entry_free(entry);
        if (errorMessage)
            *errorMessage = QObject::tr("Could not read %1").arg(fsPath);
        return false;
    }
    const QByteArray content = file.readAll();
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_size(entry, content.size());
    int wr = archive_write_header(a, entry);
    archive_entry_free(entry);
    if (wr < ARCHIVE_OK) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(archive_error_string(a));
        return false;
    }
    archive_write_data(a, content.constData(), content.size());
    return true;
}

} // namespace

bool ArchiveHandler::isSupportedArchive(const QString &path) {
    // libarchive (archive_read_support_format_all in buildTree/extract) reads all
    // of these, so listing/preview/extraction work uniformly. 7z, rar and iso are
    // read-only here (creation still offers only the tar/zip family).
    static const QStringList kExtensions = {
        ".zip",     ".tar",   ".tar.gz",  ".tgz",  ".tar.bz2", ".tbz2",
        ".tar.xz",  ".txz",   ".tar.zst", ".tzst", ".7z",      ".rar",
        ".iso",     ".deb",   ".rpm",     ".cpio", ".cab"};
    const QString lower = path.toLower();
    for (const QString &ext : kExtensions) {
        if (lower.endsWith(ext))
            return true;
    }
    // AppImages have no reliable suffix; recognise them by magic (needs unsquashfs).
    return isAppImage(path);
}

QSharedPointer<ArchiveNode> ArchiveHandler::buildTree(const QString &archivePath,
                                                       QString *errorMessage) {
    return buildTree(archivePath, QString(), nullptr, errorMessage);
}

QSharedPointer<ArchiveNode> ArchiveHandler::buildTree(const QString &archivePath,
                                                       const QString &passphrase, Status *status,
                                                       QString *errorMessage,
                                                       std::atomic<bool> *cancel) {
    auto setStatus = [&](Status s) {
        if (status)
            *status = s;
    };

    // AppImage: list the appended squashfs via unsquashfs (libarchive can't read it).
    if (isAppImage(archivePath)) {
        auto root = QSharedPointer<ArchiveNode>::create();
        root->isDir = true;
        const SquashfsReader::Status s = SquashfsReader::list(
            archivePath,
            [&](const SquashfsReader::Entry &e) {
                const QStringList parts = e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
                if (!parts.isEmpty())
                    ensurePath(root, parts, e.isDir, e.size, e.modified);
            },
            cancel);
        if (s == SquashfsReader::Status::Ok) {
            setStatus(Status::Ok);
            return root;
        }
        setStatus(Status::Error);
        if (errorMessage && errorMessage->isEmpty())
            *errorMessage = QStringLiteral("AppImage: %1").arg(int(s));
        return {};
    }

    // .7z: list via the in-process reader (encrypted + solid). Only if it can't
    // handle the container at all do we fall through to libarchive.
    if (isSevenZip(archivePath)) {
        auto root = QSharedPointer<ArchiveNode>::create();
        root->isDir = true;
        const SevenZipReader::Status s = SevenZipReader::list(
            archivePath, passphrase,
            [&](const SevenZipReader::Entry &e) {
                const QStringList parts = e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
                if (!parts.isEmpty())
                    ensurePath(root, parts, e.isDir, e.size, e.modified);
            },
            cancel);
        if (s != SevenZipReader::Status::Unsupported) {
            setStatus(sevenZipToStatus(s));
            if (s != SevenZipReader::Status::Ok) {
                if (errorMessage && errorMessage->isEmpty())
                    *errorMessage = QStringLiteral("7z: %1").arg(int(s));
                return {};
            }
            return root;
        }
        // else: unusual coder -> let libarchive try below.
    }

    // Map a libarchive data/header error string to a status: a passphrase problem
    // vs an encryption libarchive can't handle (7z/rar) vs a plain error.
    auto classify = [](const QString &e) {
        if (e.contains(QLatin1String("passphrase"), Qt::CaseInsensitive) ||
            e.contains(QLatin1String("incorrect"), Qt::CaseInsensitive) ||
            e.contains(QLatin1String("wrong password"), Qt::CaseInsensitive))
            return Status::WrongPassword;
        if (e.contains(QLatin1String("encrypted"), Qt::CaseInsensitive) &&
            e.contains(QLatin1String("not supported"), Qt::CaseInsensitive))
            return Status::EncryptedUnsupported;
        return Status::Error;
    };

    // libarchive couldn't open/decrypt this (`s`, `err`). For formats or ciphers
    // it can't do (encrypted RAR, exotic coders) try a system 7z/unrar CLI before
    // giving up; otherwise report the original libarchive status.
    auto fallback = [&](Status s, const QString &err) -> QSharedPointer<ArchiveNode> {
        if ((s == Status::Error || s == Status::EncryptedUnsupported ||
             s == Status::WrongPassword) &&
            ExternalArchiveTool::available(archivePath)) {
            auto root = QSharedPointer<ArchiveNode>::create();
            root->isDir = true;
            const ExternalArchiveTool::Status ex = ExternalArchiveTool::list(
                archivePath, passphrase,
                [&](const ExternalArchiveTool::Entry &e) {
                    const QStringList parts =
                        e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
                    if (!parts.isEmpty())
                        ensurePath(root, parts, e.isDir, e.size, e.modified);
                },
                cancel);
            if (ex == ExternalArchiveTool::Status::Ok) {
                setStatus(Status::Ok);
                return root;
            }
            if (ex == ExternalArchiveTool::Status::NeedPassword) {
                setStatus(Status::NeedPassword);
                return {};
            }
            if (ex == ExternalArchiveTool::Status::WrongPassword) {
                setStatus(Status::WrongPassword);
                return {};
            }
            // external couldn't help either -> report the original status below.
        }
        setStatus(s);
        if (errorMessage && !err.isEmpty())
            *errorMessage = err;
        return {};
    };

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    if (!passphrase.isEmpty())
        archive_read_add_passphrase(a, passphrase.toUtf8().constData());

    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        const QString e = lastArchiveError(a);
        archive_read_free(a);
        return fallback(Status::Error, e);
    }

    auto root = QSharedPointer<ArchiveNode>::create();
    root->isDir = true;

    bool sawEncrypted = false;
    bool verified = false;
    struct archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        // Abandoned while decompressing a big solid/streaming archive (the user
        // moved on): bail out so we don't keep chewing CPU in the background.
        if (cancel && cancel->load()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("cancelled");
            setStatus(Status::Error);
            archive_read_close(a);
            archive_read_free(a);
            return {};
        }
        QString entryPath = QString::fromUtf8(archive_entry_pathname(entry));
        entryPath = entryPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (entryPath.endsWith('/'))
            entryPath.chop(1);
        const bool isDir = archive_entry_filetype(entry) == AE_IFDIR;
        const QStringList parts = entryPath.split('/', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            const qint64 size = archive_entry_size(entry);
            QDateTime modified;
            if (archive_entry_mtime_is_set(entry))
                modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));
            ensurePath(root, parts, isDir, size, modified);
        }

        if (archive_entry_is_encrypted(entry))
            sawEncrypted = true;

        // With a passphrase, verify it against the first encrypted regular file
        // by actually decrypting a little of it -- a wrong password (or an
        // unsupported 7z/rar cipher) surfaces here rather than silently listing.
        if (!passphrase.isEmpty() && !verified && archive_entry_is_encrypted(entry) && !isDir) {
            char buf[4096];
            const la_ssize_t n = archive_read_data(a, buf, sizeof buf);
            if (n < 0) {
                const QString e = lastArchiveError(a);
                archive_read_close(a);
                archive_read_free(a);
                return fallback(classify(e), e);
            }
            verified = true;
        } else {
            archive_read_data_skip(a);
        }
    }

    if (r != ARCHIVE_EOF) {
        const QString e = lastArchiveError(a);
        // Header-encrypted 7z fails the whole read ("archive header is encrypted,
        // but currently not supported").
        const Status s = e.contains(QLatin1String("encrypted"), Qt::CaseInsensitive)
                             ? Status::EncryptedUnsupported
                             : Status::Error;
        archive_read_close(a);
        archive_read_free(a);
        return fallback(s, e);
    }

    archive_read_close(a);
    archive_read_free(a);

    // Encrypted but no passphrase yet -> the caller should prompt. (The names are
    // listable for ZIP, but we gate the preview behind the password like office.)
    if (sawEncrypted && passphrase.isEmpty()) {
        setStatus(Status::NeedPassword);
        return {};
    }
    setStatus(Status::Ok);
    return root;
}

bool ArchiveHandler::extract(const QString &archivePath, const QStringList &entryFullPaths,
                              const QString &destDir, QString *errorMessage) {
    return extract(archivePath, entryFullPaths, destDir, QString(), errorMessage);
}

bool ArchiveHandler::extract(const QString &archivePath, const QStringList &entryFullPaths,
                              const QString &destDir, const QString &passphrase,
                              QString *errorMessage) {
    // AppImage: extract the appended squashfs via unsquashfs.
    if (isAppImage(archivePath))
        return extractAppImage(archivePath, entryFullPaths, destDir, errorMessage);

    // .7z goes through the in-process reader (handles encrypted + solid); only a
    // container it can't decode falls through to libarchive.
    if (isSevenZip(archivePath)) {
        bool handled = false;
        const bool ok =
            extractSevenZip(archivePath, entryFullPaths, destDir, passphrase, errorMessage, &handled);
        if (handled)
            return ok;
    }

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    if (!passphrase.isEmpty())
        archive_read_add_passphrase(a, passphrase.toUtf8().constData());

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                             ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        const QString e = lastArchiveError(a);
        archive_read_free(a);
        archive_write_free(ext);
        if (ExternalArchiveTool::available(archivePath))
            return extractExternal(archivePath, entryFullPaths, destDir, passphrase, errorMessage);
        if (errorMessage)
            *errorMessage = e;
        return false;
    }

    QDir().mkpath(destDir);
    bool ok = true;
    struct archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        QString entryPath = QString::fromUtf8(archive_entry_pathname(entry));
        entryPath = entryPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (entryPath.endsWith('/'))
            entryPath.chop(1);

        bool shouldExtract = entryFullPaths.isEmpty();
        if (!shouldExtract) {
            for (const QString &sel : entryFullPaths) {
                if (entryPath == sel || entryPath.startsWith(sel + QLatin1Char('/'))) {
                    shouldExtract = true;
                    break;
                }
            }
        }
        if (!shouldExtract) {
            archive_read_data_skip(a);
            continue;
        }

        const QString destPath = QDir(destDir).filePath(entryPath);
        archive_entry_set_pathname(entry, destPath.toUtf8().constData());

        int wr = archive_write_header(ext, entry);
        if (wr < ARCHIVE_OK) {
            if (errorMessage)
                *errorMessage = lastArchiveError(ext);
            ok = false;
        } else if (archive_entry_size(entry) > 0) {
            wr = copyData(a, ext);
            if (wr < ARCHIVE_OK) {
                if (errorMessage)
                    *errorMessage = lastArchiveError(ext);
                ok = false;
            }
        }
        archive_write_finish_entry(ext);
    }

    if (r != ARCHIVE_EOF && r != ARCHIVE_OK && errorMessage && errorMessage->isEmpty())
        *errorMessage = lastArchiveError(a);

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    // libarchive couldn't fully extract (e.g. an encrypted RAR) -> retry via the
    // external CLI tool if one is installed.
    if (!ok && ExternalArchiveTool::available(archivePath)) {
        QString exErr;
        if (extractExternal(archivePath, entryFullPaths, destDir, passphrase, &exErr)) {
            if (errorMessage)
                errorMessage->clear();
            return true;
        }
    }
    return ok;
}

namespace {

// Flattens the archive tree into the full-path list ArchiveLayout::analyze wants
// (directories keep a trailing '/', matching the archive's own convention).
void collectEntryPaths(const QSharedPointer<ArchiveNode> &node, QStringList &out) {
    for (const auto &child : node->children) {
        out.append(child->isDir ? child->fullPath + QLatin1Char('/') : child->fullPath);
        if (child->isDir)
            collectEntryPaths(child, out);
    }
}

// Returns `dir` if it doesn't yet exist, otherwise "dir (2)", "dir (3)", ... so a
// smart extraction never overwrites an existing folder.
QString uniqueDir(const QString &dir) {
    if (!QFileInfo::exists(dir))
        return dir;
    for (int n = 2; n < 10000; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(dir).arg(n);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return dir;
}

} // namespace

ArchiveHandler::SmartResult ArchiveHandler::smartExtract(const QString &archivePath,
                                                          const QString &baseDestDir,
                                                          QString *errorMessage) {
    SmartResult result;

    QString err;
    const QSharedPointer<ArchiveNode> root = buildTree(archivePath, &err);
    if (!root) {
        if (errorMessage)
            *errorMessage = err;
        return result;
    }

    QStringList entryPaths;
    collectEntryPaths(root, entryPaths);

    const QString base = QFileInfo(archivePath).completeBaseName();
    const ArchiveLayout::Result layout = ArchiveLayout::analyze(entryPaths, base);

    // Multiple top-level items get wrapped in an archive-named folder; a single
    // top-level folder (or file) extracts straight into the destination.
    QString finalDir = baseDestDir;
    if (layout.wrapInArchiveNamedFolder)
        finalDir = uniqueDir(QDir(baseDestDir).filePath(base));

    if (!extract(archivePath, {}, finalDir, errorMessage))
        return result;

    result.ok = true;
    result.finalDir = finalDir;

    // Detection only: if the extracted payload is a single archive, tell the
    // caller where it landed so it can offer to unwrap it too.
    if (layout.resultIsSingleArchive && !layout.innerArchiveName.isEmpty()) {
        const QString stripped =
            layout.stripSingleRoot ? QDir(finalDir).filePath(layout.strippedPrefix) : finalDir;
        const QString nested = QDir(stripped).filePath(layout.innerArchiveName);
        if (QFileInfo::exists(nested))
            result.nestedArchivePath = nested;
    }

    return result;
}

bool ArchiveHandler::create(const QString &archivePath, const QStringList &sourcePaths,
                             const QString &format, QString *errorMessage) {
    struct archive *a = archive_write_new();

    if (format == QLatin1String("zip")) {
        archive_write_set_format_zip(a);
    } else {
        archive_write_set_format_pax_restricted(a);
        if (format == QLatin1String("tar.gz"))
            archive_write_add_filter_gzip(a);
        else if (format == QLatin1String("tar.bz2"))
            archive_write_add_filter_bzip2(a);
        else if (format == QLatin1String("tar.xz"))
            archive_write_add_filter_xz(a);
        // "tar" falls through with no compression filter.
    }

    if (archive_write_open_filename(a, archivePath.toUtf8().constData()) != ARCHIVE_OK) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(archive_error_string(a));
        archive_write_free(a);
        return false;
    }

    bool ok = true;
    for (const QString &source : sourcePaths) {
        const QString baseName = QFileInfo(source).fileName();
        if (!addEntryRecursive(a, source, baseName, errorMessage)) {
            ok = false;
            break;
        }
    }

    archive_write_close(a);
    archive_write_free(a);
    return ok;
}
