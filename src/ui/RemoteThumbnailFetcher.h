#pragma once

#include <QHash>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class FileProvider;
class QThreadPool;

// Bounded, cancellable byte-fetch service for thumbnails of files that live on
// a remote backend (SFTP/SMB/FTP/WebDAV). Browsing a share full of photos must
// not turn into hundreds of parallel downloads, so this owns the two limits
// that keep it civil: a small worker pool, so only a handful of transfers are
// ever in flight, and a cap on the backlog behind it, so a directory with
// hundreds of images doesn't queue hundreds of jobs. The icon delegate only
// asks for what it is painting, and a rejected request is simply re-asked on
// the next repaint -- by which time a slot has freed up -- so the queue tracks
// the viewport instead of the whole directory.
//
// Jobs receive a Ticket, which is both their cancellation flag and their way to
// pull bytes. All provider access happens on these worker threads through the
// same openRead/read/closeHandle contract the transfer engine uses, so each
// backend's own locking (shared session behind a mutex, or an independent
// pooled connection) applies unchanged and nothing new is assumed about their
// thread safety.
class RemoteThumbnailFetcher {
public:
    class Ticket {
    public:
        // True once the work this job was queued for has been superseded --
        // the panel navigated away, the tab disconnected, or the fetcher is
        // shutting down. Jobs poll it around every expensive step and bail out
        // instead of pulling bytes nobody will look at.
        bool cancelled() const;

        // Streams at most `maxBytes` of `path` into a fresh temp file and
        // returns its path; the caller owns that file and must remove it.
        // Returns an empty string on failure or cancellation, having removed
        // the partial file. Stopping at a budget rather than at EOF is
        // deliberate: a video only needs its head for a frame grab, and a file
        // too large to be worth fetching is better left without a thumbnail
        // than pulled down in full.
        QString download(const QString &path, qint64 maxBytes) const;

        // Fetches the first and last `halfBytes` of `path` into a sparse temp
        // file, leaving the untouched middle as a hole, and returns its path
        // (empty on failure/cancellation, partial file removed). The caller owns
        // the file.
        //
        // This is what makes a video thumbnail affordable: MP4/MOV keep their
        // index (the moov atom) either at the front or -- as ffmpeg writes by
        // default -- at the very end, and a prefix alone cannot decode the
        // latter. Writing both ends at their true offsets keeps every internal
        // byte offset valid, so the demuxer seeks correctly and reads a frame
        // from the head. Concatenating the two ends instead would shift the tail
        // and break exactly that.
        //
        // `fileSize` must be the real remote size (from the listing); a backend
        // that cannot seek falls back to a plain head-only download.
        QString downloadHeadAndTail(const QString &path, qint64 fileSize,
                                    qint64 halfBytes) const;

        // Reads at most `maxBytes` from the start of `path` straight into
        // memory, with no temp file. For probing a header -- notably a camera
        // JPEG's embedded EXIF preview, which lets a 20 MB photo be thumbnailed
        // from its first few tens of KB. Returns what it managed to read (empty
        // on failure or cancellation); a short result is not an error, since the
        // file may simply be smaller than the budget.
        QByteArray readHead(const QString &path, qint64 maxBytes) const;

        // Reads `length` bytes starting at `offset` into memory. Same contract
        // as readHead otherwise. Used to follow a container's own index to
        // wherever it points, rather than guessing how much of the file to pull.
        QByteArray readRange(const QString &path, qint64 offset, qint64 length) const;

        // Fetches the given byte ranges of `path` into a sparse temp file,
        // placing each at its true offset so the file reads as the original
        // with holes where nothing was fetched. Returns the temp path (empty on
        // failure/cancellation, partial file removed); the caller owns it.
        //
        // Keeping the real offsets is the whole point: a media file's internal
        // pointers are absolute, so the demuxer only finds its frames if every
        // byte sits where it does in the original. `fileSize` sets the file's
        // apparent length. Ranges may be given in any order and are fetched in
        // one pass where the backend allows it.
        QString downloadRanges(const QString &path, qint64 fileSize,
                               const QVector<QPair<qint64, qint64>> &ranges) const;

        // Fetches one run of bytes into a temp file that contains nothing else,
        // so the result is a short contiguous file rather than a sparse view of
        // a long one. The opposite trade to downloadRanges: absolute offsets are
        // lost, so this only suits a container the demuxer can pick up mid-file.
        //
        // That is exactly what a stream format needs. MPEG-TS has no index and
        // no global header -- a demuxer finds its way by scanning for the 0x47
        // sync byte every 188 bytes. Handed a sparse file it reads holes as
        // zeroes, fails to sync, and rescans byte by byte across the file's
        // whole apparent length: measured at 25.6 s on a 1.5 GB share file,
        // against 0.2 s for the same bytes laid out contiguously. The 15 s grab
        // timeout turns that into no thumbnail at all.
        QString downloadContiguous(const QString &path, qint64 offset, qint64 length) const;

    private:
        friend class RemoteThumbnailFetcher;
        Ticket(const RemoteThumbnailFetcher *owner, std::shared_ptr<FileProvider> provider,
               quint64 epoch);

        const RemoteThumbnailFetcher *m_owner;
        // Shared ownership, so a job still reading bytes can never outlive the
        // backend object it reads through.
        std::shared_ptr<FileProvider> m_provider;
        quint64 m_epoch;
    };

    RemoteThumbnailFetcher();
    ~RemoteThumbnailFetcher();

    RemoteThumbnailFetcher(const RemoteThumbnailFetcher &) = delete;
    RemoteThumbnailFetcher &operator=(const RemoteThumbnailFetcher &) = delete;

    using Job = std::function<void(const Ticket &)>;

    // Queues `job` against `provider`. Returns false -- having done nothing --
    // when the backlog is already full or the backend cannot stream, so the
    // caller can drop its "in progress" bookkeeping and ask again later rather
    // than leaving the request stuck forever.
    bool submit(const std::shared_ptr<FileProvider> &provider, Job job);

    // Abandons every queued and in-flight job belonging to `provider`: their
    // tickets report cancelled() from here on. Scoped per provider so one
    // panel's navigation never throws away the other panel's work.
    void cancel(const FileProvider *provider);

    // Raises or lowers how many fetches may run at once, clamped to 1..8.
    //
    // Deliberately runtime rather than a compile-time constant: the right number
    // is however many independent read channels the backend actually has. A
    // backend limited to one channel gains nothing from extra workers -- they
    // only queue deeper and hold more memory -- whereas SMB's helper
    // subprocesses give it several, and it raises this once they are confirmed
    // working. Safe to call while jobs are in flight.
    void setMaxConcurrent(int workers);

    // The current cap (post-clamp), for callers that tune it and for tests.
    int maxConcurrent() const;

    // Test seam: jobs queued or running right now.
    int outstanding() const;

private:
    bool isCancelled(const FileProvider *provider, quint64 epoch) const;

    mutable QMutex m_mutex;
    QHash<const FileProvider *, quint64> m_epochs; // bumped to cancel a provider's jobs
    int m_outstanding = 0;                          // queued + running
    bool m_shutdown = false;
    QThreadPool *m_pool; // owned; its destructor drains the queue
};
