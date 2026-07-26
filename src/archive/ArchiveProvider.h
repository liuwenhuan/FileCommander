#pragma once

#include <functional>

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QScopedPointer>
#include <QString>

#include "FileProvider.h"

class QTemporaryDir;

// A READ-ONLY FileProvider that browses an archive (zip/tar/tar.gz/... anything
// libarchive reads) as a virtual folder tree, so a FileSystemModel can enter an
// archive exactly as it enters a directory.
//
// Lazy / fast enter: the constructor reads ONLY the entry list (metadata) -- it
// never extracts on open, so even a huge archive opens instantly. The internal
// virtual tree applies the Bandizip single-root-strip (see ArchiveLayout) so the
// browse view is "smart".
//
// Reading a file's bytes (for the streaming interface used by the cross-provider
// copy engine and by preview) extracts lazily on first read:
//   * zip -> extract just the requested entry to a temp file, then serve reads
//     from it (cheap random-ish access via a sequential scan).
//   * any non-zip format (tar.*, solid 7z, ...) -> on the FIRST read of ANY
//     entry, extract the WHOLE archive to a temp dir in one pass and serve all
//     subsequent reads from there (correct for solid/streaming formats).
// A bytes-based progress callback fires during the extract-all pass so the UI
// can show a dialog.
//
// Thread-safety: list()/isDir()/... may run on a worker thread (like
// SftpProvider). The immutable virtual tree needs no locking; libarchive access
// (extraction) is serialised behind m_mutex.
class ArchiveProvider : public FileProvider {
public:
    // Reads the entry list of `archivePath` and builds the virtual tree. On
    // failure writes a reason to *error (if non-null) and leaves isValid()==false.
    //
    // `archivePath` is always a path on THIS machine's filesystem: libarchive
    // (archive_read_open_filename), unsquashfs and 7z all open an archive by
    // name, so an archive that lives on a server has to be copied down first.
    // See setExitPath()/setOwnsArchiveFile() for the two things that then differ.
    explicit ArchiveProvider(const QString &archivePath, QString *error = nullptr);
    ~ArchiveProvider() override;

    bool isValid() const { return m_valid; }

    // Where ".." at the archive root leads. Defaults to the directory the
    // archive file itself sits in, which is right whenever the user is looking
    // at that same directory. It is NOT right for an archive on a network
    // backend: the file the user double-clicked lives on the server, and the
    // local path here is a downloaded copy in /tmp, so the caller passes the
    // server-side directory (computed through the network provider, whose path
    // syntax is its own) and stepping out returns to the share.
    void setExitPath(const QString &dir) { m_exitPath = dir; }
    QString exitPath() const { return m_exitPath; }

    // Hands the archive FILE's lifetime to this provider: the destructor deletes
    // it, and the directory holding it if that leaves it empty. Set for the
    // downloaded copy of a remote archive, whose only purpose is this browse
    // session -- a multi-GB copy sitting in /tmp until the process exits is not
    // an acceptable price for having looked inside it. Never set for a local
    // archive, which is the user's own file.
    void setOwnsArchiveFile(bool owns) { m_ownsArchiveFile = owns; }

    // Suffix check: should a double-clicked file be treated as an archive?
    static bool isArchivePath(const QString &path);

    // Progress for the extract-all-on-first-read pass. Bytes done / bytes total.
    void setProgressCallback(std::function<void(qint64 done, qint64 total)> cb);

    // Extracts the file at `virtualPath` to a real temp file (keeping its name +
    // extension) and returns the local path, or "" for a directory / missing
    // entry / failure. Cached, so repeated / neighbour-prefetch calls are cheap.
    // Thread-safe (serialised on m_mutex); used to preview archived files with
    // the normal viewers.
    QString materialize(const QString &virtualPath);

    // FileProvider overrides. Paths are POSIX-style virtual archive paths rooted
    // at "/" (the archive root). An absolute path that begins with the archive's
    // own file path is also accepted and mapped to the virtual root.
    QVector<FileInfo> list(const QString &path, bool showHidden) const override;
    bool isDir(const QString &path) const override;
    QString cleanPath(const QString &path) const override;
    QString parentPath(const QString &path) const override;
    bool exists(const QString &path) const override;

    // Read-only: rename always fails.
    RenameResult rename(const QString &path, const QString &newName, QString *newPath) override;

    // Streaming READ (extract-on-demand). Writes are unsupported (read-only), so
    // openWrite/write inherit the failing defaults.
    FileHandle *openRead(const QString &path) override;
    qint64 read(FileHandle *handle, char *buffer, qint64 maxSize) override;
    bool seek(FileHandle *handle, qint64 offset) override;
    qint64 handleSize(FileHandle *handle) override;
    void closeHandle(FileHandle *handle) override;
    bool canStream() const override { return true; }

    // remove()/mkdir() inherit the failing defaults (read-only).

private:
    struct EntryMeta {
        bool isDir = false;
        qint64 size = 0;
        QDateTime modified;
        QString realPath; // path inside the archive (files only); "" for dirs
    };

    // Maps any incoming path to a normalised rooted virtual path ("/", "/a",
    // "/a/b"). Strips a leading archive-file-path prefix if present.
    QString toVirtual(const QString &path) const;

    void readEntryList(QString *error);
    void buildTree(const QString &stripPrefix);

    // Extraction (serialised on m_mutex). Return true / a temp file path.
    bool extractWhole();
    QString extractSingle(const QString &realPath);
    QString tempFilePath(const QString &realPath) const;

    QString m_archivePath;
    QString m_baseName;
    QString m_exitPath;           // "" => derive from m_archivePath (local archive)
    bool m_ownsArchiveFile = false;
    bool m_valid = false;
    bool m_extractAll = false;  // non-zip: extract everything on first read
    bool m_wholeExtracted = false;
    bool m_isSquashfs = false;  // AppImage: list/extract via unsquashfs, not libarchive
    bool m_useExternal = false; // UDF image: list/extract via the 7z tool, not libarchive

    // Raw entries read on open (pre-strip), used for tree build + extraction.
    struct RawEntry {
        QString path;
        bool isDir = false;
        qint64 size = 0;
        QDateTime modified;
    };
    QVector<RawEntry> m_rawEntries;
    qint64 m_totalBytes = 0; // sum of file sizes (for progress totals)

    // Virtual tree. Keys are rooted virtual paths; "/" is the root.
    QHash<QString, EntryMeta> m_entries;
    QHash<QString, QVector<QString>> m_children;
    // virtual path -> already-extracted temp file (per-entry zip path cache).
    mutable QHash<QString, QString> m_extractedFiles;

    QScopedPointer<QTemporaryDir> m_tempDir;

    mutable QMutex m_mutex; // serialises libarchive extraction
    std::function<void(qint64, qint64)> m_progress;
};
