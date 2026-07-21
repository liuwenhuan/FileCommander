#pragma once

#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QVector>
#include <functional>

#include "FileOpTypes.h"

class FileOperations;
class FileProvider;

// Runs local filesystem operations one at a time on a background thread so
// the UI never blocks (unchanged from the original design: one QThread, one
// FileOperations, one FIFO queue — enqueueCopy/Move/Delete/Mkdir/Rename/
// Symlink all funnel through this single pipeline exactly as before).
//
// Cross-provider transfers (enqueueProviderCopy/enqueueProviderMove) run on a
// *separate* pool of up to maxConcurrentTransfers() worker threads, each with
// its own FileOperations instance, so several SFTP/FTP/WebDAV transfers can
// be in flight at once without touching the local-job pipeline at all. With
// the default pool size of 1 (or with concurrency disabled) provider
// transfers are still fully serialised, one at a time, exactly like before
// this feature existed — only the *ordering relative to local jobs* is no
// longer a single shared FIFO, which no caller ever depended on.
//
// Conflict prompts (overwrite dialogs) are resolved by asking a GUI-thread
// handler and blocking the calling worker thread until it answers.
class OperationQueue : public QObject {
    Q_OBJECT

public:
    explicit OperationQueue(QObject *parent = nullptr);
    ~OperationQueue() override;

    // Invoked (on the GUI thread) whenever a copy/move hits an existing
    // destination file; typically wired to OverwriteConfirmDialog::ask.
    void setConflictHandler(ConflictResolver handler) { m_conflictHandler = std::move(handler); }

    // Invoked (on the GUI thread) when an entry fails to copy/delete; wire to
    // a Retry/Skip/Skip-All/Cancel dialog.
    void setErrorHandler(ErrorResolver handler) { m_errorHandler = std::move(handler); }

    void enqueueCopy(const QStringList &sources, const QString &destDir);
    void enqueueCopyAs(const QString &source, const QString &destPath);
    void enqueueMove(const QStringList &sources, const QString &destDir);
    void enqueueDelete(const QStringList &paths, bool toTrash);
    void enqueueMkdir(const QString &parentDir, const QString &name);
    void enqueueRename(const QString &path, const QString &newName);
    void enqueueSymlink(const QStringList &sources, const QString &destDir);

    // Cross-provider transfers (local<->remote) that stream through the
    // FileProvider interface with resume support. The providers are borrowed
    // (owned by the models); the queue captures the raw pointers and must not
    // outlive them. Dispatched to the concurrent transfer worker pool (see
    // setMaxConcurrentTransfers), not the local-job pipeline.
    void enqueueProviderCopy(FileProvider *src, const QStringList &sources, FileProvider *dst,
                             const QString &destDir);
    void enqueueProviderMove(FileProvider *src, const QStringList &sources, FileProvider *dst,
                             const QString &destDir);

    // Requests cancellation of all running/queued operations, both local and
    // provider transfers. Safe to call from the GUI thread; each worker stops
    // at the next per-entry boundary.
    void cancelCurrent();
    void pauseCurrent();
    void resumeCurrent();

    // Number of provider transfers allowed to run at once (>=1; clamped).
    // Growing the pool starts new worker threads immediately; shrinking it
    // only reduces how many *new* jobs are dispatched concurrently — workers
    // already running a job are left to finish rather than being killed.
    void setMaxConcurrentTransfers(int count);
    int maxConcurrentTransfers() const { return m_maxConcurrentTransfers; }

    bool isBusy() const;
    int queuedCount() const;

signals:
    void started(const QString &description);
    void queueChanged(int pendingCount); // pending jobs not yet started, local + transfers
    void progress(qint64 doneItems, qint64 totalItems, qint64 doneBytes, qint64 totalBytes,
                   const QString &currentFile);
    void errorOccurred(const QString &message);
    void finished(bool ok);

private:
    struct Job {
        QString description;
        std::function<bool(FileOperations &, QString &)> run;
    };

    // One worker in the provider-transfer pool: its own FileOperations
    // instance living on its own QThread, so its blocking network I/O never
    // shares a thread (or a curl/libssh2 session) with another transfer.
    struct TransferWorker {
        FileOperations *ops = nullptr;
        QThread *thread = nullptr;
        bool busy = false;
    };

    ErrorAction askConflict(const QString &source, const QString &destination);
    ErrorAction askError(const QString &path, const QString &error);
    void maybeStartNext();
    void onWorkerJobDone(bool ok);

    void addTransferWorker();
    void maybeStartNextTransfer();
    void onTransferWorkerJobDone(bool ok, TransferWorker *worker);

    QThread m_workerThread;
    FileOperations *m_ops;
    QQueue<Job> m_queue;
    bool m_busy = false;

    QQueue<Job> m_transferQueue;
    QVector<TransferWorker *> m_transferWorkers;
    int m_maxConcurrentTransfers = 2;

    ConflictResolver m_conflictHandler;
    ErrorResolver m_errorHandler;
};
