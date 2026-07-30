#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <memory>
#include <mutex>

class FileProvider;

class DirectorySizeTask final : public QObject {
    Q_OBJECT

public:
    DirectorySizeTask(quint64 requestId, std::shared_ptr<FileProvider> provider, QStringList roots,
                      QObject *parent = nullptr,
                      QHash<QString, qint64> symlinkRootSizes = {});
    ~DirectorySizeTask() override;

    void start();
    void cancel();

signals:
    void progress(int completedRoots, int totalRoots, qint64 bytes);
    void finished(quint64 requestId, qint64 bytes, bool cancelled);

private:
    struct ProgressUpdate {
        int completedRoots = 0;
        int totalRoots = 0;
        qint64 bytes = 0;
    };

    struct State {
        std::shared_ptr<FileProvider> provider;
        QStringList roots;
        QHash<QString, qint64> symlinkRootSizes;
        std::atomic_bool cancelled{false};
        std::mutex progressMutex;
        QVector<ProgressUpdate> progressUpdates;
    };

    struct Result {
        int completedRoots = 0;
        qint64 bytes = 0;
        bool cancelled = false;
    };

    static qint64 walkDirectory(const std::shared_ptr<State> &state, const QString &path,
                                bool *cancelled);
    void drainProgress();

    quint64 m_requestId;
    std::shared_ptr<State> m_state;
    QFutureWatcher<Result> m_watcher;
    QTimer m_progressTimer;
    bool m_started = false;
};
