#pragma once

#include <QCache>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QSet>

#include <memory>

#include "RemoteThumbnailFetcher.h"

class FileProvider;
class QThreadPool;

// Async, disk+memory cached thumbnail service backing the icon-mode file
// list. thumbnail() never blocks the calling (GUI) thread: it returns
// whatever is already available -- from the small in-memory QCache or the
// on-disk PNG cache -- and, if nothing is ready yet, schedules background
// generation on a bounded QThreadPool and emits thumbnailReady(path) once
// the result lands (in both the memory and disk cache).
//
// Images are decoded/scaled with QImageReader. Videos get one representative
// frame extracted via the system `ffmpeg` binary. Both paths run entirely on
// worker threads; only the QCache lookup/insert and signal emission touch
// the GUI thread's data (QCache itself is not thread-safe, so all access to
// it -- including from workers -- goes through m_mutex).
//
// Files on a remote backend (SFTP/SMB/FTP/WebDAV) go through the
// remoteThumbnail() overload instead: their paths mean nothing locally, so the
// bytes are streamed to a temp file first (see RemoteThumbnailFetcher, which
// owns the concurrency limit and the cancellation) and only then decoded by
// exactly the same code the local path uses.
class ThumbnailCache : public QObject {
    Q_OBJECT

public:
    static ThumbnailCache &instance();

    // What a thumbnail request did, for callers that schedule rather than
    // paint. Painting ignores this -- a null pixmap tells it everything it
    // needs -- but a background fill must tell "a result is coming" apart from
    // "try me again later", or it would either stall or spin.
    enum class Request {
        Ready,   // a cached pixmap was returned; nothing was scheduled
        Queued,  // generation is under way; thumbnailReady(path) will follow
        Busy,    // refused (the fetch backlog is full); ask again shortly
        Skipped, // nothing to do: unsupported type, bad size, over budget
    };

    // Returns a ready square pixmap (<= size px on its longest side) for
    // `path` if available (memory or disk cache); otherwise returns a null
    // QPixmap and schedules background generation, emitting
    // thumbnailReady(path) when it lands. Safe to call from the GUI thread.
    QPixmap thumbnail(const QString &path, int size);

    // The remote counterpart of thumbnail(): same contract (ready pixmap or
    // null plus a scheduled fetch), but identity comes from the listing's own
    // metadata -- `mtimeEpoch` and `fileSize` from the FileInfo the panel
    // already holds -- because there is nothing local to stat. `connectionId`
    // (the provider's "user@host") separates identical paths on different
    // servers, which "/share/a.jpg" alone would not.
    //
    // A fetch is only scheduled when the file is small enough to be worth
    // pulling (see the per-kind budgets in the .cpp); an oversized one simply
    // keeps returning null so the delegate draws the generic icon.
    QPixmap remoteThumbnail(const std::shared_ptr<FileProvider> &provider,
                            const QString &connectionId, const QString &path, qint64 mtimeEpoch,
                            qint64 fileSize, int size);

    // remoteThumbnail() with the outcome reported -- same work, same arguments.
    // Only Busy means "ask again": a background sweep filling a whole directory
    // has to tell "the queue is full right now" apart from "this row is
    // finished with", or it would either spin on a row it cannot place or drop
    // one it never fetched. The pixmap (when one was already cached) is written
    // to *ready if given.
    Request requestRemoteThumbnail(const std::shared_ptr<FileProvider> &provider,
                                   const QString &connectionId, const QString &path,
                                   qint64 mtimeEpoch, qint64 fileSize, int size,
                                   QPixmap *ready = nullptr);

    // Abandons the remote fetches queued against `provider` -- called when a
    // panel navigates away or a tab disconnects, so bytes are not pulled for a
    // listing nobody is looking at any more. In-flight work stops at its next
    // chunk boundary. Safe to call from the GUI thread (never blocks on a
    // running fetch).
    void cancelRemote(const FileProvider *provider);

    // True if `path`'s extension is a recognised image or video type --
    // i.e. a thumbnail is worth attempting. Extension-only check; does not
    // touch the filesystem.
    static bool canThumbnail(const QString &path);

signals:
    // Emitted (on the GUI thread) once a background generation attempt for
    // `path` completes, successfully or not. Listeners should re-query
    // thumbnail() -- a failed generation simply leaves it returning null.
    void thumbnailReady(const QString &path);

private:
    explicit ThumbnailCache(QObject *parent = nullptr);

    // Cache key identifying one (path, mtime, size) combination -- md5 of
    // "absolutePath\nmtimeEpoch\nsize". Two different files (or the same
    // file after being modified) never collide on the same key.
    static QString cacheKey(const QString &absolutePath, qint64 mtimeEpoch, int size);
    // Remote key. The extra fields are what a remote path lacks on its own:
    // `connectionId` disambiguates the same path on two servers, `fileSize`
    // stands in for the local stat, and the "remote:" prefix guarantees a
    // remote entry can never be mistaken for the local file at the same path.
    static QString remoteCacheKey(const QString &connectionId, const QString &path,
                                  qint64 mtimeEpoch, qint64 fileSize, int size);
    static QString diskCachePath(const QString &key);

    // Shared tail of both generation paths: looks `key` up in memory, then on
    // disk, and returns the pixmap if either hits. A disk hit is promoted into
    // the memory cache. Null means "not cached -- generate it".
    QPixmap lookupCached(const QString &key);
    // Claims `key` for generation, returning false if another request already
    // has it in flight (the caller then just waits for thumbnailReady).
    bool claimPending(const QString &key);
    void releasePending(const QString &key);

    // Runs on a worker thread: decodes/extracts the thumbnail, writes it to
    // the disk cache, then hands the result back via a queued call to
    // storeResult() on the GUI thread. Produces a QImage rather than a
    // QPixmap -- QPixmap construction/conversion is only safe on the GUI
    // thread on some platform backends, so that conversion happens in
    // storeResult() instead.
    void generate(const QString &path, const QString &key, int size);

    // Runs on a RemoteThumbnailFetcher worker: fetches only as much of the
    // remote file as a thumbnail needs, decodes it exactly as generate() does,
    // then deletes the temp file and reports through storeResult(). A failure
    // to decode is reported as a miss, leaving the generic icon in place.
    void generateRemote(const RemoteThumbnailFetcher::Ticket &ticket, const QString &path,
                        const QString &key, qint64 fileSize, int size);

    // Fetches the part of a remote video a frame grab needs, following the
    // container's own index rather than a fixed budget (see Mp4RangePlan).
    // Falls back to a fixed both-ends excerpt for formats it cannot read.
    // Returns a temp file path the caller must delete, or empty on failure.
    static QString fetchVideoExcerpt(const RemoteThumbnailFetcher::Ticket &ticket,
                                     const QString &path, qint64 fileSize);

    // GUI-thread slot (invoked via QMetaObject::invokeMethod with
    // Qt::QueuedConnection from worker threads): converts the decoded image
    // to a QPixmap, stores it in the memory cache, and notifies listeners.
    // A null `image` means generation failed; the cache miss simply persists
    // and future thumbnail() calls keep falling back to the model's icon.
    Q_INVOKABLE void storeResult(const QString &path, const QString &key, QImage image);

    QCache<QString, QPixmap> m_memCache; // cache key -> thumbnail; guarded by m_mutex
    QSet<QString> m_pending;             // cache keys currently generating; guarded by m_mutex
    QMutex m_mutex;
    QThreadPool *m_pool; // bounded worker pool; owned, but Qt manages its threads
    // Separate, much narrower pool for network fetches: a remote thumbnail is
    // bounded by the link rather than the CPU, so it gets its own limit instead
    // of competing for m_pool's threads.
    RemoteThumbnailFetcher m_remote;
};
