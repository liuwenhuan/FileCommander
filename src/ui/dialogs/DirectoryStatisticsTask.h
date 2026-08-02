#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class DirectoryStatisticsTask final : public QObject {
    Q_OBJECT

public:
    struct Result {
        qint64 bytes = 0;
        qint64 fileCount = 0;
        bool cancelled = false;
    };

    explicit DirectoryStatisticsTask(QStringList paths, QObject *parent = nullptr);
    ~DirectoryStatisticsTask() override;

    void start();
    void cancel();

    static Result scanPaths(const QStringList &paths,
                            const std::shared_ptr<std::atomic_bool> &cancelled);

signals:
    void finished(qint64 bytes, qint64 fileCount, bool cancelled);

private:
    QStringList m_paths;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFutureWatcher<Result> m_watcher;
    bool m_started = false;
};
