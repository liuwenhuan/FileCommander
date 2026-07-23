#pragma once

#include <QObject>
#include <QStringList>
#include <atomic>

// Asynchronous, cancellable recursive filename search. Runs on the Qt
// Concurrent thread pool; results stream back via resultsFound() in throttled
// batches as they're found. Batching (rather than one signal per file) keeps a
// wildcard search of a huge tree from flooding the GUI thread's event loop --
// which would otherwise starve mouse/paint events and freeze the window.
class SearchEngine : public QObject {
    Q_OBJECT

public:
    explicit SearchEngine(QObject *parent = nullptr);

    void start(const QString &rootPath, const QString &namePattern, bool caseSensitive,
               bool includeSubdirs);
    void cancel();
    bool isRunning() const { return m_running; }
    // True when the last search stopped because it hit kMaxResults rather than
    // exhausting the tree. Lets the dialog tell the user results are partial.
    bool wasTruncated() const { return m_truncated; }

    // A filename list widget cannot stay responsive with hundreds of thousands
    // of rows: insertion/layout is O(n), so the total cost is O(n^2) and the
    // window freezes. Cap the result set -- a flat search list past this size is
    // not usefully browsable anyway; the user should narrow the pattern.
    static constexpr int kMaxResults = 10000;

signals:
    void started();
    void resultsFound(const QStringList &paths);
    void finished();

private:
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_truncated{false};
};
