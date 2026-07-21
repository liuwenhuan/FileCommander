#pragma once

#include <QCache>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QSet>

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
class ThumbnailCache : public QObject {
    Q_OBJECT

public:
    static ThumbnailCache &instance();

    // Returns a ready square pixmap (<= size px on its longest side) for
    // `path` if available (memory or disk cache); otherwise returns a null
    // QPixmap and schedules background generation, emitting
    // thumbnailReady(path) when it lands. Safe to call from the GUI thread.
    QPixmap thumbnail(const QString &path, int size);

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
    static QString diskCachePath(const QString &key);

    // Runs on a worker thread: decodes/extracts the thumbnail, writes it to
    // the disk cache, then hands the result back via a queued call to
    // storeResult() on the GUI thread. Produces a QImage rather than a
    // QPixmap -- QPixmap construction/conversion is only safe on the GUI
    // thread on some platform backends, so that conversion happens in
    // storeResult() instead.
    void generate(const QString &path, const QString &key, int size);

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
};
