#pragma once

#include <QHash>
#include <QMutex>
#include <QString>

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
