#include "ArchiveProvider.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QMutexLocker>
#include <QTemporaryDir>

#include "ArchiveLayout.h"
#include "ArchiveNames.h"
#include "ArchiveVolumes.h"
#include "ExternalArchiveTool.h"
#include "SquashfsReader.h"

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
    QString p = fc::entryPathname(entry);
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

ArchiveProvider::ArchiveProvider(const QString &archivePath, QString *error,
                                 const QString &passphrase)
    : m_archivePath(archivePath), m_passphrase(passphrase) {
    // One volume of a split set is not an archive -- only the FIRST volume is,
    // and it holds the directory for all of them. Resolved before anything else
    // so every path below (including the temp-copy bookkeeping) works on the
    // file that can actually be opened.
    //
    // This cannot be left to the reader to discover: libarchive does not follow
    // a volume chain and does not say so. Measured on a real 9-volume set, a
    // middle volume opened with ARCHIVE_EOF and zero entries (a silently empty
    // archive) and the last opened with 105 unextractable fragments (a silently
    // wrong one). See ArchiveVolumes.h.
    m_volumeMember = fc::isVolumeMember(archivePath);
    if (m_volumeMember) {
        const QString first = fc::firstVolumeOf(archivePath);
        if (first.isEmpty()) {
            if (error)
                *error = QObject::tr("This is one volume of a split archive, and the first "
                                     "volume is not in this folder. Copy every volume of the "
                                     "set together before opening it.");
            return;
        }
        m_archivePath = first;
    }
    m_baseName = QFileInfo(m_archivePath).fileName();
    // An AppImage is browsed via unsquashfs (see SquashfsReader), not libarchive.
    m_isSquashfs = SquashfsReader::isAppImage(m_archivePath);
    // A UDF disc image is browsed via the 7z tool: libarchive would "succeed"
    // but only show the ISO9660 bridge stub (see preferExternal). A split set
    // goes the same way for the same reason -- libarchive cannot follow the
    // chain, and its failure mode is a plausible wrong answer rather than an
    // error, so it must not be given the chance.
    m_useExternal = !m_isSquashfs &&
                    (m_volumeMember || ExternalArchiveTool::preferExternal(m_archivePath));

    // Without the external tool there is still one kind of split we can read
    // ourselves: a RAW BYTE SPLIT (name.7z.001, ...) concatenates back into the
    // original file, so streaming the volumes in order through one reader is
    // exactly equivalent to opening the whole. A RAR set is not like that --
    // every volume carries its own headers -- so that one genuinely needs 7z or
    // unrar, and says so rather than showing a wrong tree.
    if (m_useExternal && m_volumeMember && !ExternalArchiveTool::available(m_archivePath)) {
        if (fc::isRawSplit(m_archivePath)) {
            m_volumeChain = fc::volumeChain(m_archivePath);
            m_useExternal = false;
        } else {
            if (error)
                *error = QObject::tr("This is a split archive. Reading one needs 7-Zip (or "
                                     "unrar), which was not found on this computer.");
            return;
        }
    }
    // Non-zip formats are (potentially) solid/streaming -> extract-all on first
    // read. zip -- and squashfs / 7z-on-a-disc-image, which extract a single
    // entry cheaply by name -- locate one entry without a full pass. Extracting
    // a whole disc image would also mean writing multiple GB to /tmp just to
    // preview one file.
    m_extractAll = !m_isSquashfs && !m_useExternal &&
                   !m_archivePath.toLower().endsWith(QLatin1String(".zip"));

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

ArchiveProvider::~ArchiveProvider() {
    // Drop the extracted-entry temp dir first: it may sit under the same parent
    // as an owned archive copy, and the rmdir below only succeeds once it is gone.
    m_tempDir.reset();
    if (!m_ownsArchiveFile || m_archivePath.isEmpty())
        return;
    QFile::remove(m_archivePath);
    // The downloaded copy gets a directory to itself (so it can keep the file's
    // own name); take that with it, but only if it is empty -- rmdir refuses
    // otherwise, which is exactly the guard we want against deleting anything
    // that turned out not to be ours alone.
    QDir().rmdir(QFileInfo(m_archivePath).absolutePath());
}

bool ArchiveProvider::isArchivePath(const QString &path) {
    if (ArchiveLayout::hasArchiveSuffix(path))
        return true;
    // A `.part05.rar` matches the suffix list anyway, but `.001`, `.r03` and a
    // `.part01.exe` self-extracting first volume do not -- and the SFX one is
    // the only member of its set that can actually be opened.
    if (fc::isVolumeMember(path))
        return true;
    // AppImages are recognised by magic bytes, not suffix (many have none).
    return SquashfsReader::available() && SquashfsReader::isAppImage(path);
}

void ArchiveProvider::setProgressCallback(std::function<void(qint64, qint64)> cb) {
    QMutexLocker locker(&m_mutex);
    m_progress = std::move(cb);
}

bool ArchiveProvider::openForRead(struct archive *a) const {
    // Every reader in this class opens through here, so this is also the one
    // place the passphrase has to be handed to libarchive (before the open).
    if (!m_passphrase.isEmpty())
        archive_read_add_passphrase(a, m_passphrase.toUtf8().constData());
    // A raw split is opened as the ordered list of its volumes: libarchive
    // reads them back to back as one stream, which is byte for byte the file
    // they were split from. Anything else is the single archive file.
    if (!m_volumeChain.isEmpty()) {
        QVector<QByteArray> encoded;
        QVector<const char *> names;
        encoded.reserve(m_volumeChain.size());
        names.reserve(m_volumeChain.size() + 1);
        for (const QString &volume : m_volumeChain)
            encoded.append(QFile::encodeName(volume));
        for (const QByteArray &name : encoded)
            names.append(name.constData());
        names.append(nullptr);
        return archive_read_open_filenames(a, names.data(), 10240) == ARCHIVE_OK;
    }
    const QByteArray local = QFile::encodeName(m_archivePath);
    return archive_read_open_filename(a, local.constData(), 10240) == ARCHIVE_OK;
}

void ArchiveProvider::readEntryList(QString *error) {
    // AppImage: read the appended squashfs's entry list via unsquashfs.
    if (m_isSquashfs) {
        const SquashfsReader::Status s = SquashfsReader::list(
            m_archivePath, [&](const SquashfsReader::Entry &e) {
                RawEntry r;
                r.path = e.path;
                r.isDir = e.isDir;
                r.size = e.size;
                r.modified = e.modified;
                if (!r.isDir)
                    m_totalBytes += r.size;
                m_rawEntries.append(r);
            });
        if (s != SquashfsReader::Status::Ok) {
            if (error)
                *error = s == SquashfsReader::Status::Unavailable
                             ? QStringLiteral("unsquashfs not installed")
                             : QStringLiteral("could not read AppImage filesystem");
            return;
        }
        m_valid = true;
        return;
    }

    // UDF disc image: read the real tree via 7z. Entries whose paths aren't safe
    // to materialise later are dropped here, so the tree only ever shows what
    // can actually be extracted.
    if (m_useExternal) {
        const ExternalArchiveTool::Status s = ExternalArchiveTool::list(
            m_archivePath, m_passphrase, [&](const ExternalArchiveTool::Entry &e) {
                QString path = e.path;
                while (path.endsWith(QLatin1Char('/')))
                    path.chop(1);
                if (path.isEmpty() || !SquashfsReader::isSafeEntryPath(path))
                    return;
                RawEntry r;
                r.path = path;
                r.isDir = e.isDir;
                r.size = e.isDir ? 0 : e.size;
                r.modified = e.modified;
                if (!r.isDir)
                    m_totalBytes += r.size;
                m_rawEntries.append(r);
            });
        if (s != ExternalArchiveTool::Status::Ok) {
            if (s == ExternalArchiveTool::Status::NeedPassword)
                m_status = ArchiveHandler::Status::NeedPassword;
            else if (s == ExternalArchiveTool::Status::WrongPassword)
                m_status = ArchiveHandler::Status::WrongPassword;
            if (error)
                *error = s == ExternalArchiveTool::Status::Unavailable
                             ? QStringLiteral("7z not installed")
                             : QStringLiteral("could not read disc image");
            return;
        }
        m_valid = true;
        return;
    }

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    fc::applyHeaderCharset(a);

    if (!openForRead(a)) {
        if (error)
            *error = lastArchiveError(a);
        archive_read_free(a);
        return;
    }

    struct archive_entry *entry;
    int r;
    bool sawEncrypted = false;
    bool verified = false;
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
        if (archive_entry_is_encrypted(entry))
            sawEncrypted = true;
        // With a passphrase, check it against the first encrypted file by
        // decrypting a little of it, so a wrong one is caught here rather than
        // after the tree is on screen (same probe as ArchiveHandler::buildTree).
        if (!m_passphrase.isEmpty() && !verified && !e.isDir &&
            archive_entry_is_encrypted(entry)) {
            char buf[4096];
            if (archive_read_data(a, buf, sizeof buf) < 0) {
                if (error)
                    *error = lastArchiveError(a);
                m_status = ArchiveHandler::Status::WrongPassword;
                archive_read_close(a);
                archive_read_free(a);
                return;
            }
            verified = true;
        } else {
            archive_read_data_skip(a);
        }
    }

    if (r != ARCHIVE_EOF) {
        const QString e = lastArchiveError(a);
        // A header-encrypted 7z fails the whole read ("archive header is
        // encrypted, but currently not supported") rather than listing.
        if (e.contains(QLatin1String("encrypted"), Qt::CaseInsensitive))
            m_status = ArchiveHandler::Status::EncryptedUnsupported;
        if (error && error->isEmpty())
            *error = e;
        archive_read_close(a);
        archive_read_free(a);
        return;
    }

    archive_read_close(a);
    archive_read_free(a);
    // Encrypted with nothing to decrypt it: refuse the browse and let the caller
    // ask for the password. Listing succeeded, but the contents would not.
    if (sawEncrypted && m_passphrase.isEmpty()) {
        m_status = ArchiveHandler::Status::NeedPassword;
        return;
    }
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
        // The archive root's "parent" is the directory the user came from, so the
        // panel shows a ".." that exits the archive. For a local archive that is
        // the directory the file sits in; for one browsed off a server the caller
        // has overridden it with the remote directory (setExitPath), since the
        // local path is only a downloaded copy in /tmp.
        return m_exitPath.isEmpty() ? QFileInfo(m_archivePath).absolutePath() : m_exitPath;
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

QString ArchiveProvider::materializedPathIfReady(const QString &virtualPath) const {
    const QString key = toVirtual(virtualPath);
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd() || it.value().isDir || it.value().realPath.isEmpty())
        return QString();

    // m_readyMutex, never m_mutex: the latter is held for the whole of an
    // extraction, so taking it here would make this wait out the unpack -- the
    // very freeze this method exists to avoid.
    QMutexLocker locker(&m_readyMutex);
    if (m_tempDirPath.isEmpty())
        return QString();
    if (m_extractAll) {
        return m_wholeExtracted ? QDir(m_tempDirPath).filePath(it.value().realPath)
                                : QString();
    }
    return m_extractedFiles.value(key);
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
        QMutexLocker ready(&m_readyMutex);
        m_tempDirPath = m_tempDir->path();
    }
    if (m_extractAll) {
        if (!m_wholeExtracted && !extractWhole())
            return QString();
        return tempFilePath(realPath);
    }
    QString filePath;
    {
        QMutexLocker ready(&m_readyMutex);
        filePath = m_extractedFiles.value(key);
    }
    if (filePath.isEmpty()) {
        filePath = extractSingle(realPath);
        if (filePath.isEmpty())
            return QString();
        QMutexLocker ready(&m_readyMutex);
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
    // AppImage normally uses per-entry extraction (m_extractAll is false), so this
    // path isn't taken; if it ever is, extract the whole filesystem in ONE
    // unsquashfs pass (not one process per file). unsquashfs sanitises pathnames so
    // extraction stays within the temp dir.
    if (m_isSquashfs) {
        if (SquashfsReader::extractTo(m_archivePath, {}, m_tempDir->path()) !=
            SquashfsReader::Status::Ok)
            return false;
        QMutexLocker ready(&m_readyMutex);
        m_wholeExtracted = true;
        return true;
    }

    // A UDF image always uses per-entry extraction (m_extractAll is false), so
    // this is unreachable. Guard anyway: falling through to libarchive would
    // extract the ISO9660 bridge stub and quietly serve the wrong bytes.
    if (m_useExternal)
        return false;

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    fc::applyHeaderCharset(a);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if (!openForRead(a)) {
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
        fc::setEntryPathname(entry, destPath);
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
    if (ok) {
        QMutexLocker ready(&m_readyMutex);
        m_wholeExtracted = true;
    }
    return ok;
}

QString ArchiveProvider::extractSingle(const QString &realPath) {
    // AppImage: extract the one entry via unsquashfs to a temp file. Guard the
    // write target against traversal (defense in depth -- readEntry validates too).
    if (m_isSquashfs) {
        if (!SquashfsReader::isSafeEntryPath(realPath) ||
            !SquashfsReader::isContained(m_tempDir->path(), realPath))
            return QString();
        const QString destPath = QDir(m_tempDir->path()).filePath(realPath);
        QDir().mkpath(QFileInfo(destPath).path());
        if (SquashfsReader::readEntry(m_archivePath, realPath, destPath) !=
            SquashfsReader::Status::Ok)
            return QString();
        return destPath;
    }

    // UDF disc image: pull the one entry out with 7z (same traversal guard).
    if (m_useExternal) {
        if (!SquashfsReader::isSafeEntryPath(realPath) ||
            !SquashfsReader::isContained(m_tempDir->path(), realPath))
            return QString();
        const QString destPath = QDir(m_tempDir->path()).filePath(realPath);
        QDir().mkpath(QFileInfo(destPath).path());
        // The listed size lets readEntry tell a benign non-zero exit (these
        // images always produce one) from a genuinely truncated extract.
        qint64 expected = -1;
        for (const RawEntry &e : m_rawEntries) {
            if (!e.isDir && e.path == realPath) {
                expected = e.size;
                break;
            }
        }
        if (ExternalArchiveTool::readEntry(m_archivePath, m_passphrase, realPath, destPath, nullptr,
                                           expected) != ExternalArchiveTool::Status::Ok)
            return QString();
        return destPath;
    }

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    fc::applyHeaderCharset(a);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if (!openForRead(a)) {
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
        fc::setEntryPathname(entry, destPath);
        if (archive_write_header(ext, entry) == ARCHIVE_OK) {
            // A failed copy leaves a truncated (or, for a bad passphrase, an
            // empty) file behind -- serving that path would preview garbage, so
            // report "no such entry" instead.
            bool copied = true;
            if (total > 0)
                copied = copyDataProgress(a, ext, done, total, m_progress) >= ARCHIVE_OK;
            archive_write_finish_entry(ext);
            if (copied)
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
