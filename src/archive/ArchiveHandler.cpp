#include "ArchiveHandler.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFileInfo>

#include "ArchiveLayout.h"

namespace {

QString lastArchiveError(struct archive *a) {
    const char *msg = archive_error_string(a);
    return msg ? QString::fromUtf8(msg) : QString();
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
    static const QStringList kExtensions = {".zip",    ".tar",     ".tar.gz", ".tgz",
                                             ".tar.bz2", ".tbz2",   ".tar.xz", ".txz"};
    const QString lower = path.toLower();
    for (const QString &ext : kExtensions) {
        if (lower.endsWith(ext))
            return true;
    }
    return false;
}

QSharedPointer<ArchiveNode> ArchiveHandler::buildTree(const QString &archivePath,
                                                       QString *errorMessage) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        if (errorMessage)
            *errorMessage = lastArchiveError(a);
        archive_read_free(a);
        return {};
    }

    auto root = QSharedPointer<ArchiveNode>::create();
    root->isDir = true;

    struct archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        QString entryPath = QString::fromUtf8(archive_entry_pathname(entry));
        entryPath = entryPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (entryPath.endsWith('/'))
            entryPath.chop(1);
        const QStringList parts = entryPath.split('/', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            const bool isDir = archive_entry_filetype(entry) == AE_IFDIR;
            const qint64 size = archive_entry_size(entry);
            QDateTime modified;
            if (archive_entry_mtime_is_set(entry))
                modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));
            ensurePath(root, parts, isDir, size, modified);
        }
        archive_read_data_skip(a);
    }

    if (r != ARCHIVE_EOF && errorMessage)
        *errorMessage = lastArchiveError(a);

    archive_read_close(a);
    archive_read_free(a);
    return root;
}

bool ArchiveHandler::extract(const QString &archivePath, const QStringList &entryFullPaths,
                              const QString &destDir, QString *errorMessage) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                             ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        if (errorMessage)
            *errorMessage = lastArchiveError(a);
        archive_read_free(a);
        archive_write_free(ext);
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
