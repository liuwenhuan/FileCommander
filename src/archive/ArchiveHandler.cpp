#include "ArchiveHandler.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFileInfo>

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
