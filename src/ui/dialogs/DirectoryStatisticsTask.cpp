#include "DirectoryStatisticsTask.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrent>

#include <utility>

namespace {

bool cancellationRequested(const std::shared_ptr<std::atomic_bool> &cancelled) {
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

void addFile(const QFileInfo &info, DirectoryStatisticsTask::Result *result) {
    result->bytes += info.size();
    ++result->fileCount;
}

} // namespace

DirectoryStatisticsTask::DirectoryStatisticsTask(QStringList paths, QObject *parent)
    : QObject(parent), m_paths(std::move(paths)),
      m_cancelled(std::make_shared<std::atomic_bool>(false)) {
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        const Result result = m_watcher.result();
        emit finished(result.bytes, result.fileCount, result.cancelled);
    });
}

DirectoryStatisticsTask::~DirectoryStatisticsTask() {
    cancel();
}

void DirectoryStatisticsTask::start() {
    if (m_started)
        return;
    m_started = true;

    const QStringList paths = m_paths;
    const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
    m_watcher.setFuture(QtConcurrent::run(
        [paths, cancelled] { return scanPaths(paths, cancelled); }));
}

void DirectoryStatisticsTask::cancel() {
    m_cancelled->store(true, std::memory_order_relaxed);
}

DirectoryStatisticsTask::Result DirectoryStatisticsTask::scanPaths(
    const QStringList &paths, const std::shared_ptr<std::atomic_bool> &cancelled) {
    Result result;
    for (const QString &path : paths) {
        if (cancellationRequested(cancelled)) {
            result.cancelled = true;
            return result;
        }

        const QFileInfo root(path);
        if (root.isSymLink()) {
            // A selected link is one item. In particular, never descend into a
            // directory link, which could escape the tree or form a cycle.
            addFile(root, &result);
            continue;
        }
        if (!root.isDir()) {
            if (root.exists())
                addFile(root, &result);
            continue;
        }

        QDirIterator it(path,
                        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (cancellationRequested(cancelled)) {
                result.cancelled = true;
                return result;
            }
            it.next();
            const QFileInfo entry = it.fileInfo();
            if (entry.isDir())
                continue;
            addFile(entry, &result);
        }
    }
    return result;
}
