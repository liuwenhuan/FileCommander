#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class FileProvider;
class QRegularExpression;

// One search result. The isDir flag is carried out of the traversal rather than
// recomputed by whoever consumes it: a remote hit's path is provider-internal
// (an SMB share path like "/share/docs" names nothing on this machine), so a
// local QFileInfo over it answers wrongly, and asking the provider from the GUI
// thread would block behind the very mutex the browsing session is using. The
// walk already knows the answer, so it says so.
struct SearchHit {
    QString path;
    bool isDir = false;
};
Q_DECLARE_METATYPE(SearchHit)

// Asynchronous, cancellable recursive filename search. Runs on the Qt
// Concurrent thread pool; results stream back via resultsFound() in throttled
// batches as they're found. Batching (rather than one signal per file) keeps a
// wildcard search of a huge tree from flooding the GUI thread's event loop --
// which would otherwise starve mouse/paint events and freeze the window.
//
// Two traversals, picked by whether start() is handed a provider:
//   * no provider -- the local filesystem, walked with QDirIterator. This is the
//     original path, kept verbatim because QDirIterator is markedly cheaper than
//     routing every directory through the FileProvider abstraction.
//   * a provider  -- that backend, walked through FileProvider::list(). Network
//     tabs (SMB/WebDAV/SFTP/FTP) browse provider-internal paths that do not
//     exist locally, so QDirIterator finds nothing there at all.
class SearchEngine : public QObject {
    Q_OBJECT

public:
    explicit SearchEngine(QObject *parent = nullptr);

    // `provider` null means "local filesystem" (the QDirIterator fast path).
    // Non-null means walk that backend instead; the shared_ptr is captured by
    // the worker, so the connection stays alive for the whole search even if the
    // tab that started it is closed or reconnects meanwhile -- the same
    // co-ownership RemoteThumbnailFetcher uses for the same reason.
    void start(const QString &rootPath, const QString &namePattern, bool caseSensitive,
               bool includeSubdirs, std::shared_ptr<FileProvider> provider = {});
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

    // Depth backstop for the provider walk. Symlinked directories are already
    // skipped (matching QDirIterator's default), but a misbehaving or hostile
    // server can still report a directory that contains itself under a fresh
    // name at every level, which no visited-set of paths can detect. This bounds
    // that to a finite number of round-trips. Real shares are nowhere near it.
    static constexpr int kMaxDepth = 32;

signals:
    void started();
    void resultsFound(const QVector<SearchHit> &hits);
    // The directory the walk is about to list. A remote listing can take
    // seconds, so without this the user watching an unchanged "0 found" has no
    // way to tell a slow search from a hung one. Throttled to a few per second;
    // never emitted for local searches, which are fast enough not to need it.
    void scanning(const QString &dir);
    void finished();

private:
    // The two traversals. Both share the caller's batching/cancel/cap
    // bookkeeping through `emitHit`, which returns false once the cap is hit.
    void walkLocal(const QString &rootPath, const QRegularExpression &regex, bool includeSubdirs,
                   const std::function<bool(const SearchHit &)> &emitHit);
    void walkProvider(const std::shared_ptr<FileProvider> &provider, const QString &rootPath,
                      const QRegularExpression &regex, bool includeSubdirs,
                      const std::function<bool(const SearchHit &)> &emitHit);

    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_truncated{false};
};
