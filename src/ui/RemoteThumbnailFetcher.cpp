#include "RemoteThumbnailFetcher.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRunnable>
#include <QTemporaryFile>
#include <QThreadPool>

#include "FileProvider.h"

namespace {

// Concurrent transfers are the whole point of this class: enough that a stalled
// file doesn't hold up the grid, few enough that browsing a photo share never
// looks like a download manager to the server.
//
// Two by default, because that is what a backend with a single read channel can
// actually use -- more workers on one channel only deepen the queue. Backends
// that gain real parallelism raise it at runtime via setMaxConcurrent (SMB does
// this once its helper subprocesses are confirmed available), so the worker
// count tracks the channels that exist rather than betting on them.
constexpr int kDefaultConcurrentFetches = 2;
// Ceiling for setMaxConcurrent. Past this the server, not the client, is the
// limit: measured against a real share, four workers fetch a directory of
// videos ~2x faster than one and a fifth buys nothing.
constexpr int kMaxConcurrentFetchCeiling = 8;
// Backlog cap. Painting is the producer here, so the queue only ever needs to
// hold what is on screen; anything beyond this is refused and re-requested by
// the next repaint, which keeps the queue tracking the viewport rather than
// the directory.
//
// This number is also what makes scroll "preemption" work, and it is worth
// being precise about the limit: submitted jobs run FIFO and cannot be
// reordered (QThreadPool has no priority interface), so a request already in
// the queue is never displaced. What the cap buys is that the queue is only
// ever this deep, so newly visible rows wait behind at most this many stale
// ones. That is approximate preemption, deliberately: a real priority queue
// would mean cancelling and re-submitting in-flight work for a bound that a
// small number already delivers. Raising this value directly worsens the
// worst-case wait after a scroll -- on a slow link, roughly this many fetches.
constexpr int kMaxOutstanding = 8;
// Epoch bookkeeping is per connection and connections are few; this only
// bounds the pathological case of a very long session opening and dropping
// hundreds of them.
constexpr int kMaxTrackedProviders = 64;

constexpr qint64 kChunkBytes = 64 * 1024;

} // namespace

RemoteThumbnailFetcher::Ticket::Ticket(const RemoteThumbnailFetcher *owner,
                                       std::shared_ptr<FileProvider> provider, quint64 epoch)
    : m_owner(owner), m_provider(std::move(provider)), m_epoch(epoch) {}

bool RemoteThumbnailFetcher::Ticket::cancelled() const {
    return !m_owner || m_owner->isCancelled(m_provider.get(), m_epoch);
}

QString RemoteThumbnailFetcher::Ticket::download(const QString &path, qint64 maxBytes) const {
    FileProvider *provider = m_provider.get();
    if (!provider || !provider->canStream() || maxBytes <= 0 || cancelled())
        return {};

    FileHandle *handle = provider->openRead(path);
    if (!handle)
        return {};

    // Keep the original extension on the temp file: QImageReader and ffmpeg
    // both sniff content, but a matching suffix lets them pick the right
    // decoder first try instead of probing.
    const QString suffix = QFileInfo(path).suffix();
    QString templatePath = QDir::tempPath() + QStringLiteral("/FileCommander-rthumb-XXXXXX");
    if (!suffix.isEmpty())
        templatePath += QLatin1Char('.') + suffix;
    QTemporaryFile temp(templatePath);
    temp.setAutoRemove(false); // the caller decodes it, then removes it
    if (!temp.open()) {
        provider->closeHandle(handle);
        return {};
    }
    const QString outPath = temp.fileName();

    QByteArray buffer;
    buffer.resize(kChunkBytes);
    qint64 done = 0;
    bool ok = true;
    while (done < maxBytes) {
        if (cancelled()) {
            ok = false;
            break;
        }
        const qint64 want = qMin<qint64>(buffer.size(), maxBytes - done);
        const qint64 n = provider->read(handle, buffer.data(), want);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break; // EOF -- the file was smaller than the budget
        if (temp.write(buffer.constData(), n) != n) {
            ok = false;
            break;
        }
        done += n;
    }
    provider->closeHandle(handle);
    temp.close();

    if (!ok || done == 0) {
        QFile::remove(outPath);
        return {};
    }
    return outPath;
}

QByteArray RemoteThumbnailFetcher::Ticket::readHead(const QString &path, qint64 maxBytes) const {
    FileProvider *provider = m_provider.get();
    if (!provider || !provider->canStream() || maxBytes <= 0 || cancelled())
        return {};

    FileHandle *handle = provider->openRead(path);
    if (!handle)
        return {};

    QByteArray out;
    out.reserve(static_cast<int>(qMin<qint64>(maxBytes, 1 << 20)));
    QByteArray buffer;
    buffer.resize(kChunkBytes);
    bool ok = true;
    while (out.size() < maxBytes) {
        if (cancelled()) {
            ok = false;
            break;
        }
        const qint64 want = qMin<qint64>(buffer.size(), maxBytes - out.size());
        const qint64 n = provider->read(handle, buffer.data(), want);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break; // EOF: the whole file was smaller than the budget
        out.append(buffer.constData(), static_cast<int>(n));
    }
    provider->closeHandle(handle);
    return ok ? out : QByteArray();
}

QByteArray RemoteThumbnailFetcher::Ticket::readRange(const QString &path, qint64 offset,
                                                     qint64 length) const {
    FileProvider *provider = m_provider.get();
    if (!provider || !provider->canStream() || length <= 0 || offset < 0 || cancelled())
        return {};
    if (offset == 0)
        return readHead(path, length);

    // A fresh handle per range: WebDAV and FTP only honour a seek before their
    // transfer starts (it becomes an HTTP Range / FTP REST on the next request),
    // so reusing a handle that has already read would silently return the wrong
    // bytes. Reopening costs one request and behaves the same on every backend.
    FileHandle *handle = provider->openRead(path);
    if (!handle)
        return {};
    if (!provider->seek(handle, offset)) {
        provider->closeHandle(handle);
        return {}; // backend cannot seek -- the caller falls back
    }

    QByteArray out;
    out.reserve(static_cast<int>(qMin<qint64>(length, 1 << 20)));
    QByteArray buffer;
    buffer.resize(kChunkBytes);
    bool ok = true;
    while (out.size() < length) {
        if (cancelled()) {
            ok = false;
            break;
        }
        const qint64 want = qMin<qint64>(buffer.size(), length - out.size());
        const qint64 n = provider->read(handle, buffer.data(), want);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break; // short read -- the file shrank since it was listed
        out.append(buffer.constData(), static_cast<int>(n));
    }
    provider->closeHandle(handle);
    return ok ? out : QByteArray();
}

QString RemoteThumbnailFetcher::Ticket::downloadRanges(
    const QString &path, qint64 fileSize, const QVector<QPair<qint64, qint64>> &ranges) const {
    FileProvider *provider = m_provider.get();
    if (!provider || !provider->canStream() || ranges.isEmpty() || fileSize <= 0 || cancelled())
        return {};

    const QString suffix = QFileInfo(path).suffix();
    QString templatePath = QDir::tempPath() + QStringLiteral("/FileCommander-rthumb-XXXXXX");
    if (!suffix.isEmpty())
        templatePath += QLatin1Char('.') + suffix;
    QTemporaryFile temp(templatePath);
    temp.setAutoRemove(false); // the caller decodes it, then removes it
    if (!temp.open())
        return {};
    const QString outPath = temp.fileName();
    // Give the file its real length up front so every range lands at its true
    // offset and the gaps between them stay holes, costing neither disk nor
    // network.
    if (!temp.resize(fileSize)) {
        temp.close();
        QFile::remove(outPath);
        return {};
    }

    bool wroteAnything = false;
    for (const QPair<qint64, qint64> &range : ranges) {
        if (cancelled()) {
            temp.close();
            QFile::remove(outPath);
            return {};
        }
        const qint64 offset = range.first;
        const qint64 length = qMin(range.second, fileSize - offset);
        if (offset < 0 || length <= 0 || offset >= fileSize)
            continue;

        const QByteArray chunk = readRange(path, offset, length);
        if (chunk.isEmpty())
            continue; // this range was unreachable; others may still decode
        if (!temp.seek(offset) || temp.write(chunk) != chunk.size()) {
            temp.close();
            QFile::remove(outPath);
            return {};
        }
        wroteAnything = true;
    }
    temp.close();

    if (!wroteAnything) {
        QFile::remove(outPath);
        return {};
    }
    return outPath;
}

QString RemoteThumbnailFetcher::Ticket::downloadContiguous(const QString &path, qint64 offset,
                                                            qint64 length) const {
    if (length <= 0 || offset < 0 || cancelled())
        return {};

    const QByteArray chunk = readRange(path, offset, length);
    if (chunk.isEmpty() || cancelled())
        return {};

    const QString suffix = QFileInfo(path).suffix();
    QString templatePath = QDir::tempPath() + QStringLiteral("/FileCommander-rthumb-XXXXXX");
    if (!suffix.isEmpty())
        templatePath += QLatin1Char('.') + suffix;
    QTemporaryFile temp(templatePath);
    temp.setAutoRemove(false); // the caller decodes it, then removes it
    if (!temp.open())
        return {};
    const QString outPath = temp.fileName();
    if (temp.write(chunk) != chunk.size()) {
        temp.close();
        QFile::remove(outPath);
        return {};
    }
    temp.close();
    return outPath;
}

QString RemoteThumbnailFetcher::Ticket::downloadHeadAndTail(const QString &path, qint64 fileSize,
                                                            qint64 halfBytes) const {
    // Nothing to skip: fetching both ends would just fetch the whole file, and
    // the plain path already handles that.
    if (fileSize <= halfBytes * 2)
        return download(path, fileSize);

    const QString head = download(path, halfBytes);
    if (head.isEmpty())
        return {};

    FileProvider *provider = m_provider.get();
    const qint64 tailOffset = fileSize - halfBytes;

    // A fresh handle for the tail, not a seek on the head's: WebDAV and FTP
    // refuse to seek once their transfer has started (the offset becomes an
    // HTTP Range / FTP REST on the *next* request), so reusing the handle would
    // silently read from the wrong place. Reopening costs one request and works
    // the same on every backend.
    FileHandle *handle = provider->openRead(path);
    if (!handle || !provider->seek(handle, tailOffset)) {
        if (handle)
            provider->closeHandle(handle);
        return head; // seek unsupported -- the head alone may still decode
    }

    QFile out(head);
    if (!out.open(QIODevice::ReadWrite)) {
        provider->closeHandle(handle);
        QFile::remove(head);
        return {};
    }
    // Seeking past the end and writing leaves a hole: the middle costs no
    // network and no disk, while every byte that is present sits at its true
    // offset.
    if (!out.seek(tailOffset)) {
        out.close();
        provider->closeHandle(handle);
        return head;
    }

    QByteArray buffer;
    buffer.resize(kChunkBytes);
    qint64 done = 0;
    bool ok = true;
    while (done < halfBytes) {
        if (cancelled()) {
            ok = false;
            break;
        }
        const qint64 want = qMin<qint64>(buffer.size(), halfBytes - done);
        const qint64 n = provider->read(handle, buffer.data(), want);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break; // short read -- the file shrank since it was listed
        if (out.write(buffer.constData(), n) != n) {
            ok = false;
            break;
        }
        done += n;
    }
    provider->closeHandle(handle);
    out.close();

    if (!ok) {
        QFile::remove(head);
        return {};
    }
    return head;
}

RemoteThumbnailFetcher::RemoteThumbnailFetcher() : m_pool(new QThreadPool()) {
    m_pool->setMaxThreadCount(kDefaultConcurrentFetches);
}

void RemoteThumbnailFetcher::setMaxConcurrent(int workers) {
    // QThreadPool grows and shrinks its worker set on demand, so this is safe
    // to call while jobs are in flight: raising it lets queued work start at
    // once, lowering it just stops replacing workers as they finish.
    m_pool->setMaxThreadCount(qBound(1, workers, kMaxConcurrentFetchCeiling));
}

int RemoteThumbnailFetcher::maxConcurrent() const { return m_pool->maxThreadCount(); }

RemoteThumbnailFetcher::~RemoteThumbnailFetcher() {
    {
        QMutexLocker locker(&m_mutex);
        m_shutdown = true; // queued jobs see a cancelled ticket and return at once
    }
    // ~QThreadPool waits for the queue to drain; the flag above is what keeps
    // that wait short (a running job aborts at its next chunk boundary rather
    // than pulling its whole budget down first).
    delete m_pool;
}

bool RemoteThumbnailFetcher::submit(const std::shared_ptr<FileProvider> &provider, Job job) {
    if (!provider || !job || !provider->canStream())
        return false;

    quint64 epoch = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_shutdown || m_outstanding >= kMaxOutstanding)
            return false;
        epoch = m_epochs.value(provider.get(), 0);
        ++m_outstanding;
    }

    // Size the worker pool to the parallelism this backend actually has. Asked
    // on every submit because the answer can improve: SMB only learns its real
    // channel count once a helper subprocess has connected, which happens on
    // the first read. Never lowered here -- another provider may still be using
    // the wider pool, and shrinking mid-directory would strand queued work.
    if (const int channels = provider->maxReadChannels(); channels > maxConcurrent())
        setMaxConcurrent(channels);

    // The ticket co-owns the provider, so a job that is still pulling bytes
    // cannot outlive the backend it reads through -- the reason cancellation
    // only has to stop *pointless* work, never dangling work.
    const Ticket ticket(this, provider, epoch);
    RemoteThumbnailFetcher *self = this;
    m_pool->start(QRunnable::create([self, ticket, job]() {
        job(ticket);
        QMutexLocker locker(&self->m_mutex);
        --self->m_outstanding;
    }));
    return true;
}

void RemoteThumbnailFetcher::cancel(const FileProvider *provider) {
    if (!provider)
        return;
    QMutexLocker locker(&m_mutex);
    if (m_epochs.size() > kMaxTrackedProviders)
        m_epochs.clear(); // every live ticket now mismatches: cancels the lot
    ++m_epochs[provider];
}

int RemoteThumbnailFetcher::outstanding() const {
    QMutexLocker locker(&m_mutex);
    return m_outstanding;
}

bool RemoteThumbnailFetcher::isCancelled(const FileProvider *provider, quint64 epoch) const {
    QMutexLocker locker(&m_mutex);
    return m_shutdown || m_epochs.value(provider, 0) != epoch;
}
