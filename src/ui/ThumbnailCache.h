#pragma once

#include <QCache>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QSet>

#include <memory>

#include "RemoteThumbnailFetcher.h"

class FileProvider;
class QImage;
class QThreadPool;

enum class ThumbnailDiskFormat {
    Jpeg,
    Png,
};

// Async, disk+memory cached thumbnail service backing the icon-mode file
// list. thumbnail() never blocks the calling (GUI) thread: it returns
// whatever is already available -- from the small in-memory QCache or the
// on-disk thumbnail cache -- and, if nothing is ready yet, schedules background
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

    enum class CacheIntent {
        Display,
        PersistOnly,
    };

    static constexpr int kMemoryBudgetKiB = 96 * 1024;

    struct ThumbnailMemoryStats {
        int entries = 0;
        qint64 estimatedBytes = 0;
    };

    // QCache accepts integral costs, so charge every decoded pixmap in KiB.
    // The actual dimensions and depth are used because thumbnails are not all
    // square, nor guaranteed to use the same backing format.
    static int pixmapCostKiB(const QPixmap &pixmap);

    // The on-disk representation is selected from the decoded image, before it
    // is promoted into a GUI-thread QPixmap. Opaque thumbnails use JPEG; a
    // thumbnail with visible transparency uses lossless PNG.
    static ThumbnailDiskFormat diskFormatFor(const QImage &image);
    static bool saveThumbnail(const QImage &image, const QString &key,
                              ThumbnailDiskFormat format);

    // Test-only inspection and setup for the process-wide decoded-pixmap LRU.
    void setMemoryBudgetKiBForTest(int budgetKiB);
    void insertPixmapForTest(const QString &key, const QPixmap &pixmap);
    ThumbnailMemoryStats memoryStatsForTest();
    void resetDiskDecodeCountForTest();
    int diskDecodeCountForTest() const;

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
                                   QPixmap *ready = nullptr,
                                   CacheIntent intent = CacheIntent::Display);

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

    // The pixel size the *disk* cache stores a `requested`-pixel thumbnail at:
    // `requested` rounded UP to the next rung of a fixed doubling ladder.
    //
    // This is what lets the zoom steps share work. The icon size moves in 16px
    // increments from 48 to 192 (FilePanel::zoomThumbnails), and every distinct
    // size used to mean a distinct cache key -- ten sizes, ten independent
    // regenerations of the same file, each one re-fetching a remote video and
    // re-running ffmpeg. Quantising collapses those ten onto two or three
    // rungs, so most zoom steps are served from disk.
    //
    // Rounding UP, never down, is the whole safety property: the stored bitmap
    // is always at least as large as the one being displayed, so it is only
    // ever scaled DOWN to fit. Scaling up is exactly the blur this cache was
    // just fixed to stop producing, and no reuse scheme may reintroduce it.
    //
    // Generation cost barely moves as a result: a remote fetch pulls the same
    // bytes and ffmpeg decodes the same frame whatever the target size -- only
    // the final scale-down differs. Disk cost drops outright, because ten
    // stored sizes become two.
    static int storageSize(int requested);

    // Drops the derived (possibly tinted) copies, keeping every stored bitmap.
    // Called when the phosphor tint changes: the memory keys carry the tint, so
    // stale entries would only linger until eviction otherwise.
    void invalidateMemoryCache();

    // ---- disk cache housekeeping -------------------------------------------
    //
    // The stored bitmaps are the expensive half of this cache, and nothing used
    // to remove them: the directory was flat, unindexed and unbounded, and its
    // only deletion was lookupCached() dropping a file it could not decode. Two
    // separate things then go wrong, and they need two separate answers.
    //
    //   * Every stored file becomes unreachable the moment the key format
    //     changes, and unreachable is NOT the same as deleted -- the old
    //     entries can never be hit again, so nothing ever notices they are
    //     there. purgeIfStale() is the answer: the format is stamped into the
    //     directory, and a build that disagrees with the stamp wipes it.
    //   * Reachable entries still accumulate without bound. pruneToLimit() is
    //     the answer: a cap, enforced oldest-used first.
    //
    // How fast the second one grows is easy to get wrong by an order of
    // magnitude, so it is worth stating in the units that matter. The rung
    // ladder made each stored file bigger, but it also made a file need FEWER
    // of them -- ten zoom steps used to mean ten stored bitmaps and now mean
    // two or three -- and those two effects partly cancel. The figure to size a
    // cache against is therefore the total per DISTINCT FILE, never the size of
    // one bitmap. Measured over 69 real photographs and video clips, each
    // walked across the full 48..192 zoom range (see the disabled
    // MeasuresRealCacheFootprint test, which re-derives all of this):
    //
    //     dpr 1.0      2 rungs (96,192)     26 KB per file
    //     dpr 1.25-2.0 3 rungs (96,192,384) 93 KB per file
    //     dpr 2.5      3 rungs (192,384,768) 322 KB per file

    // Wipes the whole cache directory if the format stamp written in it does
    // not match this build's (see cacheFormatStamp() in the .cpp), then writes
    // the current stamp. A no-op -- one small file read -- when they agree,
    // which is every launch but the first after a key or rung-ladder change.
    // Returns the number of files removed.
    //
    // Called from the constructor rather than deferred with the prune below,
    // because it must happen before this session writes its first thumbnail:
    // a wipe running afterwards would throw away work this session had just
    // done and force it to be redone.
    static int purgeIfStale();

    // Trims the cache back under `limitBytes` by deleting least-recently-used
    // entries -- oldest mtime first -- until it fits. Returns the number of
    // files removed; 0 when the cache was already within the limit.
    //
    // Deletion runs down to a fraction of the limit rather than to the limit
    // itself. Stopping exactly at the threshold would leave the cache sitting
    // on it, so the next launch deletes another handful, and the one after
    // that another -- a permanently thrashing cache that never gets to keep
    // anything. The gap is what makes one sweep last.
    static int pruneToLimit(qint64 limitBytes);

    // Queues one pruneToLimit() run on the worker pool `delayMs` from now,
    // against the limit from Settings. Deferred and off the GUI thread on
    // purpose: it stats every file in the directory, which has no business
    // happening on a scroll or a paint, nor competing with the rest of startup.
    // Never runs the prune more than once per process.
    void scheduleMaintenance(int delayMs = 5000);

    // The directory holding the stored bitmaps. Exposed so tests can look at
    // what housekeeping did without duplicating the path logic.
    static QString cacheDirectory();

    // The configured cap in bytes (Settings::thumbnailCacheLimitMb).
    static qint64 diskCacheLimitBytes();

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
    //
    // Two keys are derived per request and they are NOT interchangeable: the
    // *memory* key uses the exact display size, so a lookup hit can be blitted
    // 1:1 with no scaling at paint time, while the *disk* key uses
    // storageSize() so neighbouring zoom steps share one stored bitmap. The
    // memory entry is the cheap derived form; the disk entry is the expensive
    // original.
    static QString cacheKey(const QString &absolutePath, qint64 mtimeEpoch, int size);

    // Suffix identifying the active content tint, appended to the MEMORY key
    // only so a theme switch never touches the disk cache. Empty when untinted.
    static QString contentTintTag();
    // Applies the active content tint (a no-op when there is none).
    static QPixmap tintedForDisplay(const QPixmap &pixmap);
    // Remote key. The extra fields are what a remote path lacks on its own:
    // `connectionId` disambiguates the same path on two servers, `fileSize`
    // stands in for the local stat, and the "remote:" prefix guarantees a
    // remote entry can never be mistaken for the local file at the same path.
    static QString remoteCacheKey(const QString &connectionId, const QString &path,
                                  qint64 mtimeEpoch, qint64 fileSize, int size);
    static QString diskCachePath(const QString &key, ThumbnailDiskFormat format);

    // Shared tail of both generation paths: returns a ready pixmap at exactly
    // `displaySize` if one can be had without generating. Tries the memory
    // cache at `memKey` first, then the stored bitmap at `diskKey`, which is
    // scaled down to `displaySize` and promoted into memory so the next paint
    // is a plain lookup. Null means "not cached -- generate it".
    QPixmap lookupCached(const QString &memKey, const QString &diskKey, int displaySize,
                         CacheIntent intent, bool *persistedHit = nullptr);
    // Caller holds m_mutex. This is only called on the GUI thread, where
    // QPixmap creation and use are permitted.
    void insertPixmap(const QString &key, const QPixmap &pixmap);

    // Scales `image` down to fit displaySize x displaySize, preserving aspect
    // ratio. Never scales UP: a source smaller than the display box (a tiny
    // icon, or a stored bitmap that hit decodeScaled's no-upscale rule) is
    // returned untouched and simply draws at its own resolution.
    static QImage scaledForDisplay(const QImage &image, int displaySize);
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
    void generate(const QString &path, const QString &diskKey, const QString &memKey,
                  int storedSize, int displaySize);

    // Runs on a RemoteThumbnailFetcher worker: fetches only as much of the
    // remote file as a thumbnail needs, decodes it exactly as generate() does,
    // then deletes the temp file and reports through storeResult(). A failure
    // to decode is reported as a miss, leaving the generic icon in place.
    void generateRemote(const RemoteThumbnailFetcher::Ticket &ticket, const QString &path,
                        const QString &diskKey, const QString &memKey, qint64 fileSize,
                        int storedSize, int displaySize, CacheIntent intent);

    // What fetchVideoExcerpt() managed to pull.
    struct VideoExcerpt {
        QString path;           // temp file the caller must delete; empty on failure
        double seekSeconds = -1.0; // when to seek, or < 0 for "derive from duration"
    };

    // Fetches the part of a remote video a frame grab needs, following the
    // container's own index rather than a fixed budget (see Mp4RangePlan).
    // Falls back to a fixed both-ends excerpt for formats it cannot read.
    //
    // When the index named a keyframe, its timestamp comes back too so the grab
    // can seek to that frame rather than to a fixed fraction that would land in
    // an unfetched gap.
    static VideoExcerpt fetchVideoExcerpt(const RemoteThumbnailFetcher::Ticket &ticket,
                                          const QString &path, qint64 fileSize);

    // GUI-thread slot (invoked via QMetaObject::invokeMethod with
    // Qt::QueuedConnection from worker threads): converts the decoded image
    // to a QPixmap, stores it in the memory cache, and notifies listeners.
    // A null `image` means generation failed; the cache miss simply persists
    // and future thumbnail() calls keep falling back to the model's icon.
    //
    // The two keys differ deliberately: `pendingKey` is the disk-side key the
    // work was claimed under (releasing it is what lets a retry happen), while
    // `memKey` is the display-size key the already-scaled `image` belongs
    // under. Releasing the claim under the memory key would strand the file.
    Q_INVOKABLE void storeResult(const QString &path, const QString &pendingKey,
                                 const QString &memKey, QImage image, bool cacheForDisplay);

    bool m_maintenanceScheduled = false; // GUI thread only; one prune per process
    QCache<QString, QPixmap> m_memCache; // cache key -> thumbnail; guarded by m_mutex
    QSet<QString> m_pending;             // cache keys currently generating; guarded by m_mutex
    QMutex m_mutex;
    int m_diskDecodeCountForTest = 0; // GUI thread only; observes QImage disk loads in tests
    QThreadPool *m_pool; // bounded worker pool; owned, but Qt manages its threads
    // Separate, much narrower pool for network fetches: a remote thumbnail is
    // bounded by the link rather than the CPU, so it gets its own limit instead
    // of competing for m_pool's threads.
    RemoteThumbnailFetcher m_remote;
};
