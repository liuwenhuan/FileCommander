#include "SearchEngine.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrent>

#include <vector>

#include "FileInfo.h"
#include "FileProvider.h"

SearchEngine::SearchEngine(QObject *parent) : QObject(parent) {
    // resultsFound crosses from the QtConcurrent worker to the GUI thread as a
    // queued connection, which needs the payload type registered by name.
    qRegisterMetaType<QVector<SearchHit>>("QVector<SearchHit>");
}

SearchEngine::~SearchEngine() {
    m_cancelled = true;
    if (m_worker.isRunning())
        m_worker.waitForFinished();
}

void SearchEngine::cancel() {
    m_cancelled = true;
}

void SearchEngine::start(const QString &rootPath, const QString &namePattern, bool caseSensitive,
                          bool includeSubdirs, std::shared_ptr<FileProvider> provider) {
    // Drained FIRST, before any state below is set: a previous walk writes the
    // same m_cancelled/m_running/m_truncated flags on its way out, so letting it
    // overlap would have it clear the flags this call just set. Only one
    // future is kept as well, so an overlapping walk would be one the
    // destructor could not wait for.
    if (m_worker.isRunning()) {
        m_cancelled = true;
        m_worker.waitForFinished();
    }

    m_cancelled = false;
    m_running = true;
    m_truncated = false;
    emit started();

    QRegularExpression::PatternOptions options = caseSensitive
                                                      ? QRegularExpression::NoPatternOption
                                                      : QRegularExpression::CaseInsensitiveOption;
    QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(namePattern),
                              options);

    // `this` is safe to touch from the worker because the destructor waits for
    // that worker (see ~SearchEngine); it is not safe merely because callers are
    // expected to be careful. `provider` is held by value for the equivalent
    // reason on the backend's side: the tab it came from may be closed or
    // reconnected while this walk is still in flight.
    m_worker = QtConcurrent::run([this, rootPath, regex, includeSubdirs, provider]() {
        // Deliver matches in throttled batches (by count or elapsed time) rather
        // than one queued signal per file. A wildcard search of a large tree
        // matches hundreds of thousands of files; a per-file emit + addItem
        // would swamp the GUI thread's event loop so it never handles the Stop
        // click or repaints, and the window appears frozen.
        constexpr int kBatchSize = 256;
        constexpr qint64 kFlushMs = 80;
        QVector<SearchHit> batch;
        QElapsedTimer sinceFlush;
        sinceFlush.start();
        auto flush = [&]() {
            if (batch.isEmpty())
                return;
            emit resultsFound(batch);
            batch.clear();
            sinceFlush.restart();
        };

        int total = 0;
        // Returns false to tell the walk to stop: the result cap is reached and
        // continuing would only burn traversal (round-trips, on a remote share)
        // for results nobody will see.
        auto emitHit = [&](const SearchHit &hit) {
            batch.append(hit);
            if (batch.size() >= kBatchSize || sinceFlush.elapsed() >= kFlushMs)
                flush();
            if (++total >= kMaxResults) {
                // Stop at the cap: keeps the GUI list bounded and, more
                // importantly, ends the traversal so the worker isn't still
                // churning while the user reads truncated results.
                m_truncated = true;
                return false;
            }
            return true;
        };

        if (provider)
            walkProvider(provider, rootPath, regex, includeSubdirs, emitHit);
        else
            walkLocal(rootPath, regex, includeSubdirs, emitHit);
        flush();

        m_running = false;
        emit finished();
    });
}

void SearchEngine::walkLocal(const QString &rootPath, const QRegularExpression &regex,
                             bool includeSubdirs,
                             const std::function<bool(const SearchHit &)> &emitHit) {
    const QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden;
    const QDirIterator::IteratorFlags flags =
        includeSubdirs ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(rootPath, filters, flags);

    while (it.hasNext()) {
        if (m_cancelled.load())
            return;
        const QString path = it.next();
        if (!regex.match(it.fileName()).hasMatch())
            continue;
        // Only matches pay for the QFileInfo: the iterator already stat'ed the
        // entry to apply its filters, and the result count is capped, so this
        // cannot grow with the size of the tree being walked.
        if (!emitHit({path, it.fileInfo().isDir()}))
            return;
    }
}

void SearchEngine::walkProvider(const std::shared_ptr<FileProvider> &provider,
                                const QString &rootPath, const QRegularExpression &regex,
                                bool includeSubdirs,
                                const std::function<bool(const SearchHit &)> &emitHit) {
    // Explicit stack rather than recursion: a deep tree would otherwise put the
    // whole descent on this worker's (fixed-size) stack, and an explicit one is
    // also what lets the depth cap and the cancellation check be cheap.
    struct Pending {
        QString path;
        int depth;
    };
    std::vector<Pending> stack;
    stack.push_back({provider->cleanPath(rootPath), 0});

    // Guards against a directory that reaches itself -- an SMB DFS referral
    // pointing back up, or a server that resolves two names to one directory.
    // Paths are normalised first so "/a/b" and "/a//b/." are one entry.
    QSet<QString> visited;
    visited.insert(stack.front().path);

    // A remote listing can take seconds; tell the dialog which directory is
    // being walked so a slow search is visibly different from a hung one. Rate
    // limited because a share of small directories would otherwise post a
    // queued signal per directory.
    constexpr qint64 kScanNotifyMs = 250;
    QElapsedTimer sinceNotify;
    sinceNotify.start();
    bool firstDir = true;

    while (!stack.empty()) {
        // Checked here and again inside the entry loop below. This is the real
        // granularity of cancellation on a network share: a list() call is
        // synchronous and cannot be interrupted, so Stop takes effect after the
        // directory in flight returns (or hits the connection timeout) -- not
        // after the whole tree is walked, which is what matters.
        if (m_cancelled.load())
            return;

        const Pending cur = stack.back();
        stack.pop_back();

        if (firstDir || sinceNotify.elapsed() >= kScanNotifyMs) {
            emit scanning(cur.path);
            sinceNotify.restart();
            firstDir = false;
        }

        // showHidden=true mirrors the local walk's QDir::Hidden.
        //
        // Deliberately serial: every network backend guards list() with its own
        // per-connection mutex (one libsmbclient context, one curl easy handle,
        // one libssh2 session), so parallel walkers would queue on that mutex
        // anyway while holding more memory and starving the browsing session's
        // own listings. maxReadChannels() does not license concurrency here --
        // it counts independent *byte* channels for openRead (SMB's helper
        // subprocesses), which is a different resource from directory listing.
        const QVector<FileInfo> entries = provider->list(cur.path, true);

        for (const FileInfo &entry : entries) {
            if (m_cancelled.load())
                return;
            if (entry.isParentEntry())
                continue;
            if (regex.match(entry.name()).hasMatch()) {
                if (!emitHit({entry.path(), entry.isDir()}))
                    return;
            }
            if (!includeSubdirs || !entry.isDir())
                continue;
            // Don't follow symlinked directories -- QDirIterator::Subdirectories
            // doesn't either (FollowSymlinks is opt-in), so local and remote
            // searches agree, and the commonest way to build a cycle is gone.
            if (entry.isSymLink())
                continue;
            if (cur.depth + 1 > kMaxDepth)
                continue;
            const QString child = provider->cleanPath(entry.path());
            if (visited.contains(child))
                continue;
            visited.insert(child);
            stack.push_back({child, cur.depth + 1});
        }
    }
}
