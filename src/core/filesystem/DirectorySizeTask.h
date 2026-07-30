#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class FileProvider;

class DirectorySizeTask final : public QObject {
    Q_OBJECT

public:
    DirectorySizeTask(quint64 requestId, std::shared_ptr<FileProvider> provider, QStringList roots,
                      QObject *parent = nullptr);

    void start();
    void cancel();

signals:
    void progress(int completedRoots, int totalRoots, qint64 bytes);
    void finished(quint64 requestId, qint64 bytes, bool cancelled);

private:
    struct State {
        std::shared_ptr<FileProvider> provider;
        QStringList roots;
        std::atomic_bool cancelled{false};
    };

    struct Result {
        int completedRoots = 0;
        qint64 bytes = 0;
        bool cancelled = false;
    };

    static qint64 walkDirectory(const std::shared_ptr<State> &state, const QString &path,
                                bool *cancelled);

    quint64 m_requestId;
    std::shared_ptr<State> m_state;
    QFutureWatcher<Result> m_watcher;
    bool m_started = false;
};
