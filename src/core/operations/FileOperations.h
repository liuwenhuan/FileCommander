#pragma once

#include <atomic>

#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QWaitCondition>

#include "FileOpTypes.h"

class FileProvider;

// Performs the actual filesystem I/O for copy/move/delete/mkdir/rename.
// Methods here are blocking and meant to be invoked from a background
// thread (see OperationQueue); they report progress via signals, which Qt
// automatically marshals to whichever thread the receiver lives on.
class FileOperations : public QObject {
    Q_OBJECT

public:
    explicit FileOperations(QObject *parent = nullptr);

    bool copyPaths(const QStringList &sources, const QString &destDir,
                    const ConflictResolver &resolver, QString *errorMessage = nullptr);
    // Copies a single file/dir to an explicit destination path (used for
    // "copy as" / same-directory copy under a new name).
    bool copyAs(const QString &source, const QString &destPath,
                 const ConflictResolver &resolver, QString *errorMessage = nullptr);
    bool movePaths(const QStringList &sources, const QString &destDir,
                    const ConflictResolver &resolver, QString *errorMessage = nullptr);
    bool deletePaths(const QStringList &paths, bool toTrash, QString *errorMessage = nullptr);
    bool makeDirectory(const QString &parentDir, const QString &name,
                        QString *errorMessage = nullptr);
    bool renamePath(const QString &path, const QString &newName,
                     QString *errorMessage = nullptr);
    bool createSymlinks(const QStringList &sources, const QString &destDir,
                         QString *errorMessage = nullptr);

    // Cross-provider copy/move (local<->remote) with resume (断点续传). Streams
    // every file through a fixed buffer so arbitrarily large files transfer
    // without buffering the whole thing, honouring pause/cancel at each chunk
    // boundary. When a destination already holds a partial copy the transfer
    // picks up where it left off. removeSource=true performs a move (the source
    // entry is removed only after its bytes land at the destination).
    bool copyAcrossProviders(FileProvider *src, const QStringList &sources,
                             FileProvider *dst, const QString &destDir, bool removeSource,
                             const ConflictResolver &resolver, QString *errorMessage = nullptr);

    // Remote (provider) directory create + recursive delete. These mirror the
    // local makeDirectory()/deletePaths() but go through the FileProvider
    // interface, so a network tab's "new folder" / "delete" actually act on the
    // remote host instead of silently hitting the local filesystem. There is no
    // trash on a remote backend, so a provider delete is always permanent.
    bool makeProviderDirectory(FileProvider *dst, const QString &parentDir, const QString &name,
                               QString *errorMessage = nullptr);
    bool deleteProviderPaths(FileProvider *provider, const QStringList &paths,
                             QString *errorMessage = nullptr);

    bool wasCancelled() const { return m_cancelled.load(); }

    // Thread-safe: called from the GUI thread while a copy/move/delete runs
    // on the worker thread. The running loop polls m_cancelled between
    // entries and bails out at the next boundary (per-file granularity: an
    // in-flight single-file copy is not interrupted mid-write).
    void requestCancel();

    // Pause/resume between entries (same per-file granularity as cancel).
    void requestPause();
    void requestResume();

    // Optional callback consulted when an entry fails to copy/delete. Without
    // it, failures are reported and skipped (the previous behaviour).
    void setErrorResolver(ErrorResolver resolver) { m_errorResolver = std::move(resolver); }

signals:
    // doneItems/totalItems count the top-level selected entries; doneBytes/
    // totalBytes track transferred bytes (recursive) and are 0 for operations
    // where bytes are not meaningful (delete, symlink).
    void progress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                   const QString &currentFile);
    void errorOccurred(const QString &message);

private:
    bool copyOne(const QString &source, const QString &destDir, bool removeSource,
                  const ConflictResolver &resolver, ErrorAction &batchAction,
                  QString *errorMessage);
    bool copyRecursively(const QString &sourceDir, const QString &destDir);
    void emitProgress(const QString &currentFile);
    void waitIfPaused(); // blocks the worker while paused, until resume/cancel
    // Returns true if the caller should treat the failed entry as handled
    // (retried successfully is handled by the caller's loop; here Skip/SkipAll
    // return true, Retry signals retry, Cancel sets m_cancelled).
    ErrorAction resolveError(const QString &path, const QString &error);
    static qint64 countEntries(const QStringList &paths);
    static qint64 countBytes(const QStringList &paths);
    static QString uniqueDestination(const QString &destDir, const QString &name);

    // --- Cross-provider transfer internals (see copyAcrossProviders). ---
    // Per-file outcome so a move only removes the source when the copy landed.
    enum class FileResult { Done, Skipped, Failed };
    // Copies (recursing into directories) one source entry to destPath.
    bool transferEntry(FileProvider *src, const QString &srcPath, FileProvider *dst,
                       const QString &destPath, bool removeSource,
                       const ConflictResolver &resolver, ErrorAction &batchAction,
                       QString *errorMessage);
    // Copies a single regular file, applying conflict resolution and resume.
    FileResult transferFile(FileProvider *src, const QString &srcPath, FileProvider *dst,
                            const QString &destPath, const ConflictResolver &resolver,
                            ErrorAction &batchAction, QString *errorMessage);
    // Streams one file's bytes; startOffset>0 resumes an interrupted transfer.
    bool streamCopy(FileProvider *src, const QString &srcPath, FileProvider *dst,
                    const QString &destPath, bool truncate, qint64 startOffset,
                    QString *failMsg);
    // Recursively removes a remote entry (depth-first) via the provider; used by
    // deleteProviderPaths. Honours pause/cancel and the error resolver per node.
    bool removeProviderTree(FileProvider *provider, const QString &path, QString *errorMessage);
    // Recursively totals the byte size of a source tree via list()/handle sizes.
    static qint64 countProviderBytes(FileProvider *src, const QStringList &paths);
    static qint64 providerTreeBytes(FileProvider *src, const QString &path);
    static qint64 providerFileSize(FileProvider *provider, const QString &path);
    static QString uniqueProviderDestination(FileProvider *dst, const QString &destPath);
    static QString joinPath(const QString &dir, const QString &name);
    static QString lastComponent(const QString &path);

    std::atomic<bool> m_cancelled{false};
    QMutex m_pauseMutex;
    QWaitCondition m_pauseCond;
    bool m_paused = false;
    ErrorResolver m_errorResolver;
    ErrorAction m_errorBatch = ErrorAction::Retry; // sentinel: ask each time
    qint64 m_totalItems = 0;
    qint64 m_doneItems = 0;
    qint64 m_totalBytes = 0;
    qint64 m_doneBytes = 0;
};
