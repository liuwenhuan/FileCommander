#include "DirectorySizeTask.h"

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
                                     QStringList roots, QObject *parent,
                                     QHash<QString, qint64> symlinkRootSizes)
    : QObject(parent), m_requestId(requestId), m_state(std::make_shared<State>()) {
    m_state->provider = std::move(provider);
    m_state->roots = std::move(roots);
    m_state->symlinkRootSizes = std::move(symlinkRootSizes);
    m_progressTimer.setInterval(10);
    connect(&m_progressTimer, &QTimer::timeout, this, &DirectorySizeTask::drainProgress);
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        m_progressTimer.stop();
        drainProgress();
        const Result result = m_watcher.result();
        emit finished(m_requestId, result.bytes, result.cancelled);
    });
}

DirectorySizeTask::~DirectorySizeTask() {
    cancel();
}

void DirectorySizeTask::start() {
    if (m_started)
        return;
    m_started = true;

    const std::shared_ptr<State> state = m_state;
    m_progressTimer.start();
    m_watcher.setFuture(QtConcurrent::run([state] {
        Result result;
        const int totalRoots = state->roots.size();
        for (const QString &root : state->roots) {
            if (state->cancelled.load(std::memory_order_relaxed)) {
                result.cancelled = true;
                break;
            }

            bool cancelled = false;
            const auto symlinkSize = state->symlinkRootSizes.constFind(root);
            const qint64 rootBytes = symlinkSize == state->symlinkRootSizes.cend()
                                         ? DirectorySizeTask::walkDirectory(state, root, &cancelled)
                                         : symlinkSize.value();
            if (cancelled) {
                result.cancelled = true;
                break;
            }

            result.bytes += rootBytes;
            ++result.completedRoots;
            {
                std::lock_guard<std::mutex> lock(state->progressMutex);
                state->progressUpdates.append(
                    {result.completedRoots, totalRoots, result.bytes});
            }
        }
        result.cancelled = result.cancelled || state->cancelled.load(std::memory_order_relaxed);
        return result;
    }));
}

void DirectorySizeTask::cancel() {
    m_state->cancelled.store(true, std::memory_order_relaxed);
}

void DirectorySizeTask::drainProgress() {
    QVector<ProgressUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(m_state->progressMutex);
        updates.swap(m_state->progressUpdates);
    }
    for (const ProgressUpdate &update : updates)
        emit progress(update.completedRoots, update.totalRoots, update.bytes);
}
