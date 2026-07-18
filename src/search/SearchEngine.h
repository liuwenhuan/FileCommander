#pragma once

#include <QObject>
#include <atomic>

// Asynchronous, cancellable recursive filename search. Runs on the Qt
// Concurrent thread pool; results stream back via resultFound() as they're
// found rather than batching until the end.
class SearchEngine : public QObject {
    Q_OBJECT

public:
    explicit SearchEngine(QObject *parent = nullptr);

    void start(const QString &rootPath, const QString &namePattern, bool caseSensitive,
               bool includeSubdirs);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void started();
    void resultFound(const QString &path);
    void finished();

private:
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
};
