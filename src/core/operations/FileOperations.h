#pragma once

#include <atomic>

#include <QDateTime>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QWaitCondition>

#include "FileOpTypes.h"
#include "FileProvider.h"
#include "privilege/PrivilegeBroker.h"

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
    bool deletePaths(const QStringList &paths, bool toTrash, QString *errorMessage = nullptr,
                     QStringList *trashUndoEntries = nullptr);
    bool restoreTrashEntries(const QStringList &entries, QString *errorMessage = nullptr);
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
    // on the worker thread. Every loop polls m_cancelled between entries, and
    // the local single-file copy primitive polls it *within* one file as well
    // (see copyFilePreservingTime), so a stop request no longer has to wait for
    // a multi-gigabyte file to finish before anything reacts to it.
    void requestCancel();

    // Pause/resume between entries (same per-file granularity as cancel).
    void requestPause();
    void requestResume();

    // Byte-rate cap (bytes/second) for cross-provider streaming copies. 0 (the
    // default) disables pacing; otherwise streamCopy() sleeps between chunks so
    // the average rate over a file stays at or under this. Per-transfer, not
    // shared across workers. Pacing sleeps in short slices so a cancel still
    // lands promptly.
    void setTransferRateLimit(qint64 bytesPerSecond);

    // The interruption checkpoint reached from inside one file's copy: fires the
    // test hook and answers whether the copy should give up now. Public only
    // because the Win32 progress routine that consults it has to be a plain
    // function; nothing outside the copy primitives should call it.
    bool copyInterrupted(qint64 copiedBytes, qint64 totalBytes);

    // Optional callback consulted when an entry fails to copy/delete. Without
    // it, failures are reported and skipped (the previous behaviour).
    void setErrorResolver(ErrorResolver resolver) { m_errorResolver = std::move(resolver); }
    void setErrorResolver(LegacyErrorResolver resolver) {
        m_errorResolver = [resolver = std::move(resolver)](const OperationError &error) {
            return resolver(error.sourcePath, error.message);
        };
    }
    void setPrivilegeExecutor(
        std::function<PrivilegeResult(const PrivilegedOperationRequest &)> executor) {
        m_privilegeExecutor = std::move(executor);
    }
    void setNativeErrorOverrideForTesting(
        std::function<qint64(OperationType, const QString &, const QString &, qint64)> override) {
        m_nativeErrorOverrideForTesting = std::move(override);
    }
    // Called from inside the local single-file copy every time the primitive
    // reaches a point where it could still be interrupted, with the bytes
    // copied so far and the file's total size. Exists so a test can park the
    // worker *mid-file* and request cancellation from there, which is the only
    // way to prove the interruption path is real rather than the copy simply
    // finishing before the request lands.
    void setCopyChunkHookForTesting(std::function<void(qint64, qint64)> hook) {
        m_copyChunkHookForTesting = std::move(hook);
    }

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
    bool copyRecursively(const QString &sourceDir, const QString &destDir,
                         qint64 *nativeCode);
    bool copyDirectorySafely(const QString &sourceDir, const QString &destDir,
                             bool overwrite, qint64 *nativeCode);
    // The single local file-copy primitive: replaces `target`, copies `source`
    // onto it, and carries the source's modification time across. Every local
    // copy goes through here so the timestamp behaviour can't diverge between
    // the flat, recursive, and copy-as paths.
    //
    // Returns only whether the *copy* succeeded. Restamping the time is
    // best-effort and deliberately cannot fail the operation: on a filesystem
    // that won't accept it, the result is a copied file with a fresh timestamp,
    // which is exactly the pre-existing behaviour.
    //
    // Interruptible: the copy polls m_cancelled as it runs and gives up
    // part-way through, deleting the partial destination it was writing (see
    // copyFileChunked / the Win32 progress routine). A cancelled copy reports
    // failure with the platform's "operation aborted" code, and callers must
    // check m_cancelled before treating that as an error worth reporting.
    bool copyFilePreservingTime(const QString &source, const QString &target,
                                bool overwrite, qint64 *nativeCode);
#ifndef Q_OS_WIN
    // POSIX single-file copy loop. QFile::copy cannot be interrupted once it
    // starts, so the bytes are moved here in chunks with a cancel checkpoint
    // between them. Matches QFile::copy's contract: fails (without touching it)
    // when `target` already exists, and carries the source's permissions over.
    bool copyFileChunked(const QString &source, const QString &target, qint64 *nativeCode);
#endif
    void emitProgress(const QString &currentFile, qint64 displayDoneBytes = -1);
    void waitIfPaused(); // blocks the worker while paused, until resume/cancel
    void paceTransfer(); // throttles the last chunk to m_rateLimitBps (no-op at 0)
    // Returns true if the caller should treat the failed entry as handled
    // (retried successfully is handled by the caller's loop; here Skip/SkipAll
    // return true, Retry signals retry, Cancel sets m_cancelled).
    ErrorAction resolveError(const OperationError &error);
    ErrorAction resolveError(OperationType operation, const QString &sourcePath,
                             const QString &targetPath, qint64 nativeCode,
                             const QString &message, bool localOperation);
    enum class FailureResolution { Retry, Elevated, Failed };
    FailureResolution resolveLocalFailure(OperationError error,
                                          const PrivilegedOperationRequest &request);
    qint64 overrideNativeErrorForTesting(OperationType operation,
                                         const QString &sourcePath,
                                         const QString &targetPath,
                                         qint64 nativeCode) const;
    static qint64 countEntries(const QStringList &paths);
    static qint64 countBytes(const QStringList &paths);
    static QString uniqueDestination(const QString &destDir, const QString &name);

    // --- Cross-provider transfer internals (see copyAcrossProviders). ---
    // Per-file outcome so a move only removes the source when the copy landed.
    enum class FileResult { Done, Skipped, Failed };

    // Result of asking a backend to move an entry on the server. Collapses the
    // provider's four-way answer into what the transfer loop actually decides
    // between. Occupied is kept apart from Unavailable because it says nothing
    // about later entries -- only that this one needs conflict resolution.
    enum class MoveOutcome { Moved, Occupied, Unavailable };
    // Attempts a whole-entry server-side move (no bytes through the client).
    // Never reports an error: anything other than success means the caller
    // proceeds with the streaming copy it would have done anyway.
    MoveOutcome tryServerSideMove(FileProvider *provider, const QString &srcPath,
                                  const QString &destPath);
    // Copies (recursing into directories) one source entry to destPath.
    //
    // `sourceTime` is the source's modification time when the caller already
    // knows it, so the copy can be restamped without asking the backend again.
    // A directory listing hands out every child's time for free, so recursion
    // passes it down; only a top-level file (which arrives as a bare path) leaves
    // it invalid and pays for a lookup.
    bool transferEntry(FileProvider *src, const QString &srcPath, FileProvider *dst,
                       const QString &destPath, bool removeSource,
                       const ConflictResolver &resolver, ErrorAction &batchAction,
                       QString *errorMessage, const QDateTime &sourceTime = {});
    // Copies a single regular file, applying conflict resolution and resume.
    FileResult transferFile(FileProvider *src, const QString &srcPath, FileProvider *dst,
                            const QString &destPath, const ConflictResolver &resolver,
                            ErrorAction &batchAction, QString *errorMessage,
                            const QDateTime &sourceTime = {});
    // Streams one file's bytes; startOffset>0 resumes an interrupted transfer.
    bool streamCopy(FileProvider *src, const QString &srcPath, FileProvider *dst,
                    const QString &destPath, bool truncate, qint64 startOffset,
                    QString *failMsg, const QDateTime &sourceTime = {},
                    FileHandle::StreamError *closeError = nullptr);
    // Recursively removes a remote entry (depth-first) via the provider; used by
    // deleteProviderPaths. Honours pause/cancel and the error resolver per node.
    bool removeProviderTree(FileProvider *provider, const QString &path, QString *errorMessage);
    // Recursively totals the byte size of a source tree via list()/handle sizes.
    static qint64 countProviderBytes(FileProvider *src, const QStringList &paths);
    static qint64 providerTreeBytes(FileProvider *src, const QString &path);
    static qint64 providerFileSize(FileProvider *provider, const QString &path);
    // The file's modification time as its backend reports it, or an invalid
    // QDateTime if it can't be determined.
    //
    // There is no single-file stat in the FileProvider interface, so this lists
    // the parent directory -- one extra round trip on a network backend. Callers
    // that already hold the entry's FileInfo must pass its modified() down
    // instead of calling this (see transferEntry's sourceTime); otherwise copying
    // a directory of N files would re-list its parent N times.
    static QDateTime providerFileModified(FileProvider *provider, const QString &path);
    // Whether the first `prefixLen` bytes of srcPath and destPath are identical,
    // read through their providers. The only way to know a partial destination
    // was written from this same source: sizes alone cannot tell a file that was
    // edited in place between two attempts from one that merely grew by append.
    static bool prefixMatches(FileProvider *src, const QString &srcPath, FileProvider *dst,
                              const QString &destPath, qint64 prefixLen);
    static QString uniqueProviderDestination(FileProvider *dst, const QString &destPath);
    static QString joinPath(const QString &dir, const QString &name);
    static QString lastComponent(const QString &path);

    std::atomic<bool> m_cancelled{false};
    // The write handle of the transfer currently streaming on this worker, so
    // requestCancel() can tell the backend to stop now rather than waiting for
    // the copy loop to reach its next checkpoint. Guarded by m_handleMutex
    // because the cancel arrives on the GUI thread while the loop runs here.
    QMutex m_handleMutex;
    FileHandle *m_activeWriteHandle = nullptr;
    qint64 m_rateLimitBps = 0;   // 0 = unlimited
    QElapsedTimer m_rateClock;   // paces one file's chunks
    qint64 m_rateBytes = 0;      // bytes transferred since m_rateClock started
    QMutex m_pauseMutex;
    QWaitCondition m_pauseCond;
    bool m_paused = false;
    ErrorResolver m_errorResolver;
    std::function<PrivilegeResult(const PrivilegedOperationRequest &)> m_privilegeExecutor;
    std::function<qint64(OperationType, const QString &, const QString &, qint64)>
        m_nativeErrorOverrideForTesting;
    std::function<void(qint64, qint64)> m_copyChunkHookForTesting;
    ErrorAction m_errorBatch = ErrorAction::Retry; // sentinel: ask each time
    qint64 m_totalItems = 0;
    qint64 m_doneItems = 0;
    qint64 m_totalBytes = 0;
    qint64 m_doneBytes = 0;
};
