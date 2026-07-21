#include "ArchiveProvider.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTemporaryDir>

#include "ArchiveLayout.h"

namespace {

// Wraps a QFile over an extracted temp file so it travels through FileHandle.
struct ArchiveReadHandle : FileHandle {
    QFile file;
    explicit ArchiveReadHandle(const QString &path) : file(path) {}
};

QString lastArchiveError(struct archive *a) {
    const char *msg = archive_error_string(a);
    return msg ? QString::fromUtf8(msg) : QString();
}

// Normalises a libarchive pathname: '\\' -> '/', drop trailing slashes.
QString normalisedEntryPath(struct archive_entry *entry) {
    QString p = QString::fromUtf8(archive_entry_pathname(entry));
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

QString basenameOf(const QString &virtualPath) {
    const int slash = virtualPath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? virtualPath : virtualPath.mid(slash + 1);
}

} // namespace

ArchiveProvider::ArchiveProvider(const QString &archivePath, QString *error)
    : m_archivePath(archivePath) {
    m_baseName = QFileInfo(archivePath).fileName();
    // Non-zip formats are (potentially) solid/streaming -> extract-all on first
    // read. zip supports locating a single entry cheaply enough via a scan.
    m_extractAll = !m_archivePath.toLower().endsWith(QLatin1String(".zip"));

    readEntryList(error);
    if (!m_valid)
        return;

    QStringList entryPaths;
    entryPaths.reserve(m_rawEntries.size());
    for (const RawEntry &e : m_rawEntries)
        entryPaths.append(e.isDir ? e.path + QLatin1Char('/') : e.path);

    const ArchiveLayout::Result layout = ArchiveLayout::analyze(entryPaths, m_baseName);
    const QString stripPrefix =
        layout.stripSingleRoot ? layout.strippedPrefix + QLatin1Char('/') : QString();
    buildTree(stripPrefix);
}

ArchiveProvider::~ArchiveProvider() = default;

bool ArchiveProvider::isArchivePath(const QString &path) {
    return ArchiveLayout::hasArchiveSuffix(path);
}

void ArchiveProvider::setProgressCallback(std::function<void(qint64, qint64)> cb) {
    QMutexLocker locker(&m_mutex);
    m_progress = std::move(cb);
}

void ArchiveProvider::readEntryList(QString *error) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, m_archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        if (error)
            *error = lastArchiveError(a);
        archive_read_free(a);
        return;
    }

    struct archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const QString path = normalisedEntryPath(entry);
        if (path.isEmpty()) {
            archive_read_data_skip(a);
            continue;
        }
        RawEntry e;
        e.path = path;
        e.isDir = archive_entry_filetype(entry) == AE_IFDIR;
        e.size = archive_entry_size(entry);
        if (archive_entry_mtime_is_set(entry))
            e.modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));
        if (!e.isDir)
            m_totalBytes += e.size;
        m_rawEntries.append(e);
        archive_read_data_skip(a);
    }

    if (r != ARCHIVE_EOF) {
        if (error && error->isEmpty())
            *error = lastArchiveError(a);
        archive_read_close(a);
        archive_read_free(a);
        return;
    }

    archive_read_close(a);
    archive_read_free(a);
    m_valid = true;
}

void ArchiveProvider::buildTree(const QString &stripPrefix) {
    m_entries.insert(QStringLiteral("/"), EntryMeta{true, 0, QDateTime(), QString()});

    for (const RawEntry &raw : m_rawEntries) {
        // Map the real archive path to its virtual path by removing the stripped
        // single-root prefix. The stripped directory itself maps to the root.
        QString virtualBody;
        if (!stripPrefix.isEmpty()) {
            if (raw.path + QLatin1Char('/') == stripPrefix || raw.path == stripPrefix)
                continue; // the stripped root dir entry itself -> root, skip
            if (raw.path.startsWith(stripPrefix))
                virtualBody = raw.path.mid(stripPrefix.size());
            else
                continue; // shouldn't happen once a single root was detected
        } else {
            virtualBody = raw.path;
        }
        if (virtualBody.isEmpty())
            continue;

        const QStringList parts = virtualBody.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString parentKey = QStringLiteral("/");
        for (int i = 0; i < parts.size(); ++i) {
            const bool isLast = (i == parts.size() - 1);
            const QString childKey = parentKey == QStringLiteral("/")
                                         ? QStringLiteral("/") + parts.at(i)
                                         : parentKey + QLatin1Char('/') + parts.at(i);
            if (!m_entries.contains(childKey)) {
                EntryMeta meta;
                meta.isDir = isLast ? raw.isDir : true;
                m_entries.insert(childKey, meta);
                m_children[parentKey].append(childKey);
            }
            if (isLast) {
                EntryMeta &meta = m_entries[childKey];
                meta.isDir = raw.isDir;
                meta.size = raw.size;
                meta.modified = raw.modified;
                if (!raw.isDir)
                    meta.realPath = raw.path;
            }
            parentKey = childKey;
        }
    }
}

QString ArchiveProvider::toVirtual(const QString &path) const {
    QString p = path;
    // Accept an absolute path that begins with the archive file path.
    if (p.startsWith(m_archivePath))
        p = p.mid(m_archivePath.size());
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // POSIX normalisation independent of the local platform.
    const QStringList rawParts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList stack;
    for (const QString &part : rawParts) {
        if (part == QStringLiteral(".")) {
            continue;
        } else if (part == QStringLiteral("..")) {
            if (!stack.isEmpty())
                stack.removeLast();
        } else {
            stack.append(part);
        }
    }
    if (stack.isEmpty())
        return QStringLiteral("/");
    return QLatin1Char('/') + stack.join(QLatin1Char('/'));
}

QVector<FileInfo> ArchiveProvider::list(const QString &path, bool showHidden) const {
    const QString key = toVirtual(path);
    QVector<FileInfo> result;
    const auto it = m_children.constFind(key);
    if (it == m_children.constEnd())
        return result;

    for (const QString &childKey : it.value()) {
        const EntryMeta &meta = m_entries.value(childKey);
        const QString name = basenameOf(childKey);
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            continue;
        const QFile::Permissions perms =
            meta.isDir ? (QFile::ReadOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup |
                          QFile::ReadOther | QFile::ExeOther)
                       : (QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);
        result.append(
            FileInfo::fromFields(childKey, name, meta.size, meta.modified, meta.isDir, perms));
    }
    return result;
}

bool ArchiveProvider::isDir(const QString &path) const {
    const auto it = m_entries.constFind(toVirtual(path));
    return it != m_entries.constEnd() && it.value().isDir;
}

QString ArchiveProvider::cleanPath(const QString &path) const { return toVirtual(path); }

QString ArchiveProvider::parentPath(const QString &path) const {
    const QString clean = toVirtual(path);
    if (clean == QStringLiteral("/"))
        // The archive root's "parent" is the local directory the archive lives
        // in, so the panel shows a ".." that exits the archive back to disk.
        return QFileInfo(m_archivePath).absolutePath();
    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QStringLiteral("/");
    return clean.left(slash);
}

bool ArchiveProvider::exists(const QString &path) const {
    return m_entries.contains(toVirtual(path));
}

FileProvider::RenameResult ArchiveProvider::rename(const QString &, const QString &, QString *) {
    return RenameResult::Failed; // read-only
}

QString ArchiveProvider::tempFilePath(const QString &realPath) const {
    return QDir(m_tempDir->path()).filePath(realPath);
}

FileHandle *ArchiveProvider::openRead(const QString &path) {
    const QString key = toVirtual(path);
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd() || it.value().isDir || it.value().realPath.isEmpty())
        return nullptr;
    const QString realPath = it.value().realPath;

    QMutexLocker locker(&m_mutex);
    if (!m_tempDir) {
        m_tempDir.reset(new QTemporaryDir);
        if (!m_tempDir->isValid())
            return nullptr;
    }

    QString filePath;
    if (m_extractAll) {
        if (!m_wholeExtracted && !extractWhole())
            return nullptr;
        filePath = tempFilePath(realPath);
    } else {
        filePath = m_extractedFiles.value(key);
        if (filePath.isEmpty()) {
            filePath = extractSingle(realPath);
            if (filePath.isEmpty())
                return nullptr;
            m_extractedFiles.insert(key, filePath);
        }
    }

    auto *h = new ArchiveReadHandle(filePath);
    if (!h->file.open(QIODevice::ReadOnly)) {
        delete h;
        return nullptr;
    }
    return h;
}

qint64 ArchiveProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    auto *h = static_cast<ArchiveReadHandle *>(handle);
    return h ? h->file.read(buffer, maxSize) : -1;
}

bool ArchiveProvider::seek(FileHandle *handle, qint64 offset) {
    auto *h = static_cast<ArchiveReadHandle *>(handle);
    return h && h->file.seek(offset);
}

qint64 ArchiveProvider::handleSize(FileHandle *handle) {
    auto *h = static_cast<ArchiveReadHandle *>(handle);
    return h ? h->file.size() : -1;
}

void ArchiveProvider::closeHandle(FileHandle *handle) {
    delete static_cast<ArchiveReadHandle *>(handle);
}

QString ArchiveProvider::materialize(const QString &virtualPath) {
    const QString key = toVirtual(virtualPath);
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd() || it.value().isDir || it.value().realPath.isEmpty())
        return QString();
    const QString realPath = it.value().realPath;

    QMutexLocker locker(&m_mutex);
    if (!m_tempDir) {
        m_tempDir.reset(new QTemporaryDir);
        if (!m_tempDir->isValid())
            return QString();
    }
    if (m_extractAll) {
        if (!m_wholeExtracted && !extractWhole())
            return QString();
        return tempFilePath(realPath);
    }
    QString filePath = m_extractedFiles.value(key);
    if (filePath.isEmpty()) {
        filePath = extractSingle(realPath);
        if (filePath.isEmpty())
            return QString();
        m_extractedFiles.insert(key, filePath);
    }
    return filePath;
}

// --- Extraction ------------------------------------------------------------
// Both routines assume m_mutex is held and m_tempDir is valid.

namespace {

// Copies one entry's data blocks from ar -> aw, accumulating bytes into `done`
// and firing the progress callback (if any) after each block.
int copyDataProgress(struct archive *ar, struct archive *aw, qint64 &done, qint64 total,
                     const std::function<void(qint64, qint64)> &progress) {
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
        done += static_cast<qint64>(size);
        if (progress)
            progress(done, total);
    }
}

} // namespace

bool ArchiveProvider::extractWhole() {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, m_archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        return false;
    }

    const QString dest = m_tempDir->path();
    QDir().mkpath(dest);
    bool ok = true;
    qint64 done = 0;
    if (m_progress)
        m_progress(0, m_totalBytes);

    struct archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const QString entryPath = normalisedEntryPath(entry);
        if (entryPath.isEmpty()) {
            archive_read_data_skip(a);
            continue;
        }
        const QString destPath = QDir(dest).filePath(entryPath);
        archive_entry_set_pathname(entry, destPath.toUtf8().constData());
        int wr = archive_write_header(ext, entry);
        if (wr < ARCHIVE_OK) {
            ok = false;
        } else if (archive_entry_size(entry) > 0) {
            if (copyDataProgress(a, ext, done, m_totalBytes, m_progress) < ARCHIVE_OK)
                ok = false;
        }
        archive_write_finish_entry(ext);
    }
    if (r != ARCHIVE_EOF)
        ok = false;

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    if (ok)
        m_wholeExtracted = true;
    return ok;
}

QString ArchiveProvider::extractSingle(const QString &realPath) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, m_archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        return QString();
    }

    const QString dest = m_tempDir->path();
    QDir(dest).mkpath(QFileInfo(QDir(dest).filePath(realPath)).path());
    QString resultPath;
    qint64 done = 0;

    struct archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const QString entryPath = normalisedEntryPath(entry);
        if (entryPath != realPath) {
            archive_read_data_skip(a);
            continue;
        }
        const QString destPath = QDir(dest).filePath(entryPath);
        const qint64 total = archive_entry_size(entry);
        archive_entry_set_pathname(entry, destPath.toUtf8().constData());
        if (archive_write_header(ext, entry) == ARCHIVE_OK) {
            if (total > 0)
                copyDataProgress(a, ext, done, total, m_progress);
            archive_write_finish_entry(ext);
            resultPath = destPath;
        }
        break; // found our entry; stop scanning
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return resultPath;
}
