#include "OperationQueue.h"

#include <QFileInfo>

#include "FileOperations.h"

OperationQueue::OperationQueue(QObject *parent) : QObject(parent) {
    m_ops = new FileOperations();
    m_ops->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_ops, &QObject::deleteLater);
    connect(m_ops, &FileOperations::progress, this, &OperationQueue::progress);
    connect(m_ops, &FileOperations::errorOccurred, this, &OperationQueue::errorOccurred);
    m_ops->setErrorResolver(
        [this](const QString &path, const QString &error) { return askError(path, error); });
    m_workerThread.start();
}

OperationQueue::~OperationQueue() {
    m_workerThread.quit();
    m_workerThread.wait();
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

void OperationQueue::cancelCurrent() {
    // Drop everything not yet started so the queue doesn't keep going after
    // the user cancels, then signal the in-flight job to stop.
    m_queue.clear();
    if (m_ops)
        m_ops->requestCancel();
}

void OperationQueue::maybeStartNext() {
    if (m_busy || m_queue.isEmpty())
        return;
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

void OperationQueue::onWorkerJobDone(bool ok) {
    m_busy = false;
    emit finished(ok);
    maybeStartNext();
}
