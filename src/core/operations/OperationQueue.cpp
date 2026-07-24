#include "OperationQueue.h"

#include <QFileInfo>

#include "FileOperations.h"
#include "FileProvider.h"
#include "Settings.h"

OperationQueue::OperationQueue(QObject *parent) : QObject(parent) {
    m_ops = new FileOperations();
    m_ops->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_ops, &QObject::deleteLater);
    connect(m_ops, &FileOperations::progress, this, &OperationQueue::progress);
    connect(m_ops, &FileOperations::errorOccurred, this, &OperationQueue::errorOccurred);
    m_ops->setErrorResolver(
        [this](const QString &path, const QString &error) { return askError(path, error); });
    m_workerThread.start();

    m_maxConcurrentTransfers = Settings().maxConcurrentTransfers();
    for (int i = 0; i < m_maxConcurrentTransfers; ++i)
        addTransferWorker();
}

OperationQueue::~OperationQueue() {
    m_workerThread.quit();
    m_workerThread.wait();

    for (TransferWorker *worker : qAsConst(m_transferWorkers)) {
        worker->thread->quit();
        worker->thread->wait();
        delete worker->thread;
        delete worker;
    }
    m_transferWorkers.clear();
}

ErrorAction OperationQueue::askConflict(const QString &source, const QString &destination) {
    ErrorAction result = ErrorAction::Skip;
    QMetaObject::invokeMethod(
        this,
        [this, &source, &destination, &result]() {
            result = m_conflictHandler ? m_conflictHandler(source, destination)
                                        : ErrorAction::Skip;
        },
        Qt::BlockingQueuedConnection);
    return result;
}

void OperationQueue::enqueueCopy(const QStringList &sources, const QString &destDir) {
    Job job;
    job.description = tr("Copying %1 item(s) to %2").arg(sources.size()).arg(destDir);
    ConflictResolver resolver = [this](const QString &s, const QString &d) {
        return askConflict(s, d);
    };
    job.run = [sources, destDir, resolver](FileOperations &ops, QString &err) {
        return ops.copyPaths(sources, destDir, resolver, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

ErrorAction OperationQueue::askError(const QString &path, const QString &error) {
    ErrorAction result = ErrorAction::Skip;
    QMetaObject::invokeMethod(
        this,
        [this, &path, &error, &result]() {
            result = m_errorHandler ? m_errorHandler(path, error) : ErrorAction::Skip;
        },
        Qt::BlockingQueuedConnection);
    return result;
}

void OperationQueue::enqueueCopyAs(const QString &source, const QString &destPath) {
    Job job;
    job.description = tr("Copying %1").arg(QFileInfo(source).fileName());
    ConflictResolver resolver = [this](const QString &s, const QString &d) {
        return askConflict(s, d);
    };
    job.run = [source, destPath, resolver](FileOperations &ops, QString &err) {
        return ops.copyAs(source, destPath, resolver, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

void OperationQueue::enqueueMove(const QStringList &sources, const QString &destDir) {
    Job job;
    job.description = tr("Moving %1 item(s) to %2").arg(sources.size()).arg(destDir);
    ConflictResolver resolver = [this](const QString &s, const QString &d) {
        return askConflict(s, d);
    };
    job.run = [sources, destDir, resolver](FileOperations &ops, QString &err) {
        return ops.movePaths(sources, destDir, resolver, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

void OperationQueue::enqueueDelete(const QStringList &paths, bool toTrash) {
    Job job;
    job.description = tr("Deleting %1 item(s)").arg(paths.size());
    job.run = [paths, toTrash](FileOperations &ops, QString &err) {
        return ops.deletePaths(paths, toTrash, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

void OperationQueue::enqueueMkdir(const QString &parentDir, const QString &name) {
    Job job;
    job.description = tr("Creating directory %1").arg(name);
    job.run = [parentDir, name](FileOperations &ops, QString &err) {
        return ops.makeDirectory(parentDir, name, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

void OperationQueue::enqueueRename(const QString &path, const QString &newName) {
    Job job;
    job.description = tr("Renaming %1").arg(path);
    job.run = [path, newName](FileOperations &ops, QString &err) {
        return ops.renamePath(path, newName, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

void OperationQueue::enqueueSymlink(const QStringList &sources, const QString &destDir) {
    Job job;
    job.description = tr("Linking %1 item(s) into %2").arg(sources.size()).arg(destDir);
    job.run = [sources, destDir](FileOperations &ops, QString &err) {
        return ops.createSymlinks(sources, destDir, &err);
    };
    m_queue.enqueue(job);
    maybeStartNext();
}

void OperationQueue::enqueueProviderCopy(FileProvider *src, const QStringList &sources,
                                         FileProvider *dst, const QString &destDir) {
    Job job;
    job.description = tr("Copying %1 item(s) to %2").arg(sources.size()).arg(destDir);
    ConflictResolver resolver = [this](const QString &s, const QString &d) {
        return askConflict(s, d);
    };
    // Capture the borrowed provider pointers by value (raw pointer copy) — the
    // models own them; the job must not take ownership.
    job.run = [src, sources, dst, destDir, resolver](FileOperations &ops, QString &err) {
        return ops.copyAcrossProviders(src, sources, dst, destDir, /*removeSource=*/false,
                                       resolver, &err);
    };
    m_transferQueue.enqueue(job);
    maybeStartNextTransfer();
}

void OperationQueue::enqueueProviderMove(FileProvider *src, const QStringList &sources,
                                         FileProvider *dst, const QString &destDir) {
    Job job;
    job.description = tr("Moving %1 item(s) to %2").arg(sources.size()).arg(destDir);
    ConflictResolver resolver = [this](const QString &s, const QString &d) {
        return askConflict(s, d);
    };
    job.run = [src, sources, dst, destDir, resolver](FileOperations &ops, QString &err) {
        return ops.copyAcrossProviders(src, sources, dst, destDir, /*removeSource=*/true,
                                       resolver, &err);
    };
    m_transferQueue.enqueue(job);
    maybeStartNextTransfer();
}

void OperationQueue::enqueueProviderMkdir(FileProvider *dst, const QString &parentDir,
                                          const QString &name) {
    Job job;
    job.description = tr("Creating directory %1").arg(name);
    job.run = [dst, parentDir, name](FileOperations &ops, QString &err) {
        return ops.makeProviderDirectory(dst, parentDir, name, &err);
    };
    m_transferQueue.enqueue(job);
    maybeStartNextTransfer();
}

void OperationQueue::enqueueProviderDelete(FileProvider *provider, const QStringList &paths) {
    Job job;
    job.description = tr("Deleting %1 item(s)").arg(paths.size());
    job.run = [provider, paths](FileOperations &ops, QString &err) {
        return ops.deleteProviderPaths(provider, paths, &err);
    };
    m_transferQueue.enqueue(job);
    maybeStartNextTransfer();
}

void OperationQueue::enqueueProviderRename(FileProvider *provider, const QString &path,
                                           const QString &newName) {
    Job job;
    job.description = tr("Renaming %1").arg(path);
    job.run = [provider, path, newName](FileOperations &, QString &err) {
        QString newPath;
        if (provider->rename(path, newName, &newPath) == FileProvider::RenameResult::Ok)
            return true;
        err = tr("Failed to rename %1").arg(path);
        return false;
    };
    m_transferQueue.enqueue(job);
    maybeStartNextTransfer();
}

void OperationQueue::cancelCurrent() {
    // Drop everything not yet started so the queue doesn't keep going after
    // the user cancels, then signal the in-flight job(s) to stop — both the
    // local pipeline and every provider-transfer worker.
    m_queue.clear();
    if (m_ops)
        m_ops->requestCancel();

    m_transferQueue.clear();
    for (TransferWorker *worker : qAsConst(m_transferWorkers)) {
        if (worker->ops)
            worker->ops->requestCancel();
    }

    emit queueChanged(m_queue.size() + m_transferQueue.size());
}

void OperationQueue::pauseCurrent() {
    if (m_ops)
        m_ops->requestPause();
    for (TransferWorker *worker : qAsConst(m_transferWorkers)) {
        if (worker->ops)
            worker->ops->requestPause();
    }
}

void OperationQueue::resumeCurrent() {
    if (m_ops)
        m_ops->requestResume();
    for (TransferWorker *worker : qAsConst(m_transferWorkers)) {
        if (worker->ops)
            worker->ops->requestResume();
    }
}

void OperationQueue::setMaxConcurrentTransfers(int count) {
    const int clamped = qBound(1, count, 8);
    m_maxConcurrentTransfers = clamped;
    while (m_transferWorkers.size() < clamped)
        addTransferWorker();
    // Shrinking never kills a worker mid-job; it just stops handing out new
    // jobs to the extra workers once they finish (maybeStartNextTransfer only
    // dispatches to workers while the pool size allows it implicitly, since
    // idle workers beyond the new limit simply won't be given more work here
    // because there are, in steady state, never more queued jobs than the
    // pool can absorb faster than the limit — dispatch below re-checks
    // m_maxConcurrentTransfers each time).
    maybeStartNextTransfer();
}

bool OperationQueue::isBusy() const {
    if (m_busy)
        return true;
    for (const TransferWorker *worker : m_transferWorkers) {
        if (worker->busy)
            return true;
    }
    return false;
}

int OperationQueue::queuedCount() const {
    return m_queue.size() + m_transferQueue.size();
}

void OperationQueue::maybeStartNext() {
    if (!m_busy && !m_queue.isEmpty()) {
        m_busy = true;
        Job job = m_queue.dequeue();
        emit started(job.description);

        QMetaObject::invokeMethod(
            m_ops,
            [this, job]() {
                QString err;
                bool ok = job.run(*m_ops, err);
                QMetaObject::invokeMethod(
                    this, [this, ok]() { onWorkerJobDone(ok); }, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    emit queueChanged(m_queue.size() + m_transferQueue.size());
}

void OperationQueue::onWorkerJobDone(bool ok) {
    m_busy = false;
    emit finished(ok);
    maybeStartNext();
}

void OperationQueue::addTransferWorker() {
    auto *worker = new TransferWorker();
    worker->thread = new QThread();
    worker->ops = new FileOperations();
    worker->ops->moveToThread(worker->thread);
    connect(worker->thread, &QThread::finished, worker->ops, &QObject::deleteLater);
    connect(worker->ops, &FileOperations::progress, this, &OperationQueue::progress);
    connect(worker->ops, &FileOperations::errorOccurred, this, &OperationQueue::errorOccurred);
    worker->ops->setErrorResolver(
        [this](const QString &path, const QString &error) { return askError(path, error); });
    worker->thread->start();
    m_transferWorkers.append(worker);
}

void OperationQueue::maybeStartNextTransfer() {
    // Hand queued transfer jobs out to every idle worker within the current
    // pool-size limit, so several provider transfers run at once (bounded by
    // m_maxConcurrentTransfers). With a pool of size 1 this degenerates to
    // exactly the same one-at-a-time behaviour as the local-job pipeline.
    for (int i = 0; i < m_transferWorkers.size() && i < m_maxConcurrentTransfers; ++i) {
        TransferWorker *worker = m_transferWorkers[i];
        if (worker->busy || m_transferQueue.isEmpty())
            continue;

        worker->busy = true;
        Job job = m_transferQueue.dequeue();
        emit started(job.description);

        QMetaObject::invokeMethod(
            worker->ops,
            [this, worker, job]() {
                QString err;
                bool ok = job.run(*worker->ops, err);
                QMetaObject::invokeMethod(
                    this, [this, ok, worker]() { onTransferWorkerJobDone(ok, worker); },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    emit queueChanged(m_queue.size() + m_transferQueue.size());
}

void OperationQueue::onTransferWorkerJobDone(bool ok, TransferWorker *worker) {
    worker->busy = false;
    emit finished(ok);
    maybeStartNextTransfer();
}
