#include "DirectorySizeTask.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrent>

#include "FileProvider.h"

qint64 DirectorySizeTask::walkDirectory(const std::shared_ptr<State> &state, const QString &path,
                                        bool *cancelled) {
    if (state->cancelled.load(std::memory_order_relaxed)) {
        *cancelled = true;
        return 0;
    }

    const QVector<FileInfo> entries = state->provider->list(path, /*showHidden=*/true);
    if (state->cancelled.load(std::memory_order_relaxed)) {
        *cancelled = true;
        return 0;
    }

    qint64 total = 0;
    for (const FileInfo &entry : entries) {
        if (state->cancelled.load(std::memory_order_relaxed)) {
            *cancelled = true;
            return 0;
        }
        if (entry.isParentEntry())
            continue;
        // Never follow a directory symlink. Apart from local loops, a provider
        // cannot promise that a link stays on the same backend or permission set.
        if (entry.isDir() && !entry.isSymLink()) {
            total += DirectorySizeTask::walkDirectory(state, entry.path(), cancelled);
            if (*cancelled)
                return 0;
        } else {
            total += entry.size();
        }
    }
    return total;
}

DirectorySizeTask::DirectorySizeTask(quint64 requestId, std::shared_ptr<FileProvider> provider,
                                     QStringList roots, QObject *parent)
    : QObject(parent), m_requestId(requestId), m_state(std::make_shared<State>()) {
    m_state->provider = std::move(provider);
    m_state->roots = std::move(roots);
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        const Result result = m_watcher.result();
        emit finished(m_requestId, result.bytes, result.cancelled);
    });
}

void DirectorySizeTask::start() {
    if (m_started)
        return;
    m_started = true;

    const std::shared_ptr<State> state = m_state;
    const QPointer<DirectorySizeTask> task(this);
    QObject *const eventLoop = QCoreApplication::instance();
    m_watcher.setFuture(QtConcurrent::run([state, task, eventLoop] {
        Result result;
        const int totalRoots = state->roots.size();
        for (const QString &root : state->roots) {
            if (state->cancelled.load(std::memory_order_relaxed)) {
                result.cancelled = true;
                break;
            }

            bool cancelled = false;
            const qint64 rootBytes = DirectorySizeTask::walkDirectory(state, root, &cancelled);
            if (cancelled) {
                result.cancelled = true;
                break;
            }

            result.bytes += rootBytes;
            ++result.completedRoots;
            if (eventLoop) {
                QMetaObject::invokeMethod(
                    eventLoop,
                    [task, completed = result.completedRoots, totalRoots, bytes = result.bytes] {
                        if (task)
                            emit task->progress(completed, totalRoots, bytes);
                    },
                    Qt::QueuedConnection);
            }
        }
        result.cancelled = result.cancelled || state->cancelled.load(std::memory_order_relaxed);
        return result;
    }));
}

void DirectorySizeTask::cancel() {
    m_state->cancelled.store(true, std::memory_order_relaxed);
}
