#include "MpvStreamSource.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QUrl>

#include <mpv/client.h>
#include <mpv/stream_cb.h>

#include <cstring>

#include "FileProvider.h"

namespace MpvStreamSource {

const char *const kScheme = "fcstream";

namespace {

// A gap smaller than this is read through and thrown away rather than paid for
// with a fresh connection. Sized against the read pattern mpv actually
// produces: it asks in 64 KB-451 KB chunks, so a sub-megabyte skip is roughly
// two chunks of transfer against one connection setup plus a round trip.
constexpr qint64 kForwardSkipLimit = 1 << 20; // 1 MiB

// Block cache geometry. 64 KB blocks are small enough that pulling one to serve
// a 12 KB probe is not wasteful, and large enough that a cluster of probes
// within the same region collapses into a single fetch.
constexpr qint64 kBlockSize = 64 * 1024;
// Reads at or below this go through the cache; anything larger is streaming
// playback and would only churn it.
constexpr qint64 kSmallRead = 64 * 1024;

constexpr qint64 kSkipChunk = 64 * 1024;

struct Entry {
    std::shared_ptr<FileProvider> provider;
    QString path;
};

// Published URLs, keyed by token.
//
// Deliberately never destroyed: libmpv may call the open callback -- and so
// reach this table -- from its own threads right up until mpv_terminate_destroy
// returns, which happens inside MpvWidget's destructor. A function-local static
// object would be a race against static destruction order; a leaked one cannot
// be.
class Registry {
public:
    QString add(const std::shared_ptr<FileProvider> &provider, const QString &path) {
        QMutexLocker locker(&m_mutex);
        const QString token = QString::number(++m_next);
        m_entries.insert(token, Entry{provider, path});
        m_order.append(token);
        // Keep only the most recent handful. An evicted URL cannot be opened
        // again, but a stream already reading through it holds its own
        // reference to the provider and carries on unaffected.
        while (m_order.size() > kMaxPublished) {
            const QString old = m_order.takeFirst();
            m_entries.remove(old);
        }
        return token;
    }

    bool lookup(const QString &token, Entry *out) const {
        QMutexLocker locker(&m_mutex);
        const auto it = m_entries.constFind(token);
        if (it == m_entries.constEnd())
            return false;
        *out = it.value();
        return true;
    }

    void remove(const QString &token) {
        QMutexLocker locker(&m_mutex);
        m_entries.remove(token);
        m_order.removeAll(token);
    }

private:
    static constexpr int kMaxPublished = 8;

    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    QStringList m_order; // publication order, for the bound above
    quint64 m_next = 0;
};

Registry &registry() {
    static Registry *r = new Registry; // intentionally leaked; see above
    return *r;
}

// Open stream count. Leaked for the same reason as the registry: a stream can
// outlive static destruction, and its destructor decrements this.
std::atomic<int> &liveStreams() {
    static auto *count = new std::atomic<int>(0);
    return *count;
}

QString schemePrefix() {
    return QString::fromLatin1(kScheme) + QStringLiteral("://");
}

// "fcstream://<token>/<name>" -> "<token>". Empty if the URL isn't ours.
QString tokenOf(const QString &url) {
    const QString prefix = schemePrefix();
    if (!url.startsWith(prefix))
        return QString();
    const QString rest = url.mid(prefix.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    return slash < 0 ? rest : rest.left(slash);
}

// --- libmpv callbacks ------------------------------------------------------
// These run on mpv's demuxer thread (measured: read/seek/size/close all arrive
// on one and the same thread for the life of a stream), except cancel, which
// mpv documents as coming from a different thread and forbids from blocking.

int64_t cbRead(void *cookie, char *buf, uint64_t nbytes) {
    return static_cast<Stream *>(cookie)->read(buf, static_cast<qint64>(nbytes));
}

int64_t cbSeek(void *cookie, int64_t offset) {
    const qint64 result = static_cast<Stream *>(cookie)->seek(offset);
    return result < 0 ? MPV_ERROR_GENERIC : result;
}

int64_t cbSize(void *cookie) {
    const qint64 size = static_cast<Stream *>(cookie)->size();
    return size < 0 ? MPV_ERROR_UNSUPPORTED : size;
}

void cbClose(void *cookie) {
    delete static_cast<Stream *>(cookie);
}

void cbCancel(void *cookie) {
    static_cast<Stream *>(cookie)->cancel();
}

int cbOpen(void *, char *uri, mpv_stream_cb_info *info) {
    Entry entry;
    if (!registry().lookup(tokenOf(QString::fromUtf8(uri)), &entry))
        return MPV_ERROR_LOADING_FAILED;
    info->cookie = new Stream(entry.provider, entry.path);
    info->read_fn = cbRead;
    info->seek_fn = cbSeek;
    info->size_fn = cbSize;
    info->close_fn = cbClose;
    info->cancel_fn = cbCancel;
    return 0;
}

} // namespace

bool registerProtocol(mpv_handle *mpv) {
    return mpv && mpv_stream_cb_add_ro(mpv, kScheme, nullptr, cbOpen) >= 0;
}

QString publish(const std::shared_ptr<FileProvider> &provider, const QString &remotePath) {
    if (!provider || !provider->canStream() || remotePath.isEmpty())
        return QString();
    const QString token = registry().add(provider, remotePath);
    // The trailing name is decoration with one job: it carries the extension,
    // which is how both mpv and QuickView::isVideo recognise the format. It is
    // percent-encoded so a space or a '#' in a filename cannot break the URL,
    // and it is never parsed back -- the token alone identifies the stream.
    const QString name =
        QString::fromLatin1(QUrl::toPercentEncoding(QFileInfo(remotePath).fileName()));
    return schemePrefix() + token + QLatin1Char('/') + name;
}

bool isStreamUrl(const QString &path) {
    return path.startsWith(schemePrefix());
}

void revoke(const QString &url) {
    const QString token = tokenOf(url);
    if (!token.isEmpty())
        registry().remove(token);
}

int activeStreams() {
    return liveStreams().load();
}

// --- Stream ----------------------------------------------------------------

Stream::Stream(std::shared_ptr<FileProvider> provider, QString remotePath)
    : m_provider(std::move(provider)), m_path(std::move(remotePath)) {
    ++liveStreams();
}

Stream::~Stream() {
    closeHandle();
    --liveStreams();
}

void Stream::closeHandle() {
    if (m_handle) {
        m_provider->closeHandle(static_cast<FileHandle *>(m_handle));
        m_handle = nullptr;
    }
}

bool Stream::skipForward(qint64 target) {
    QByteArray scratch;
    scratch.resize(static_cast<int>(kSkipChunk));
    while (m_handlePos < target) {
        if (m_cancelled.load())
            return false;
        const qint64 want = qMin<qint64>(scratch.size(), target - m_handlePos);
        const qint64 n =
            m_provider->read(static_cast<FileHandle *>(m_handle), scratch.data(), want);
        if (n <= 0)
            return false;
        m_handlePos += n;
        m_fetched += n; // these bytes crossed the wire, even though nobody wanted them
    }
    return true;
}

bool Stream::positionTo(qint64 target) {
    if (m_cancelled.load())
        return false;

    if (!m_handle) {
        m_handle = m_provider->openRead(m_path);
        if (!m_handle)
            return false;
        ++m_opens;
        m_handlePos = 0;
    }
    if (m_handlePos == target)
        return true;

    // Cheapest first: ask the backend to seek. SMB and SFTP always can. WebDAV
    // and FTP can only while their transfer hasn't started, and refusing costs
    // nothing -- it is a flag test, not a request -- so there is no reason not
    // to try.
    if (m_provider->seek(static_cast<FileHandle *>(m_handle), target)) {
        m_handlePos = target;
        return true;
    }

    // Refused. A short hop forward is cheaper to read through than to pay a
    // connection setup and a round trip for.
    if (target > m_handlePos && target - m_handlePos <= kForwardSkipLimit &&
        skipForward(target))
        return true;

    // Otherwise start the transfer again at the offset we want. This is the
    // same answer RemoteThumbnailFetcher reaches for a ranged read.
    closeHandle();
    m_handle = m_provider->openRead(m_path);
    if (!m_handle)
        return false;
    ++m_opens;
    m_handlePos = 0;
    if (target != 0 && !m_provider->seek(static_cast<FileHandle *>(m_handle), target))
        return false; // a backend that cannot seek at all can't serve this read
    m_handlePos = target;
    return true;
}

int Stream::findBlock(qint64 offset) const {
    for (int i = 0; i < kBlockCount; ++i)
        if (m_blocks[i].offset == offset)
            return i;
    return -1;
}

int Stream::fetchBlock(qint64 offset) {
    if (!positionTo(offset))
        return -1;

    QByteArray data;
    data.resize(static_cast<int>(kBlockSize));
    qint64 got = 0;
    while (got < kBlockSize) {
        if (m_cancelled.load())
            return -1;
        const qint64 n = m_provider->read(static_cast<FileHandle *>(m_handle),
                                          data.data() + got, kBlockSize - got);
        if (n < 0)
            return -1;
        if (n == 0)
            break; // EOF inside the block; keep the short one
        got += n;
        m_handlePos += n;
        m_fetched += n;
    }
    if (got == 0)
        return -1;
    data.resize(static_cast<int>(got));

    // Evict least-recently-used.
    int victim = 0;
    for (int i = 1; i < kBlockCount; ++i)
        if (m_blocks[i].stamp < m_blocks[victim].stamp)
            victim = i;
    m_blocks[victim].offset = offset;
    m_blocks[victim].data = data;
    m_blocks[victim].stamp = ++m_clock;
    return victim;
}

qint64 Stream::read(char *buffer, qint64 maxSize) {
    if (m_cancelled.load())
        return -1;
    if (maxSize <= 0)
        return 0;
    if (m_sizeKnown && m_size >= 0 && m_pos >= m_size)
        return 0; // genuine EOF

    // Small reads go through the block cache; big ones are sequential playback
    // and are handed straight to the backend.
    if (maxSize <= kSmallRead) {
        const qint64 blockOffset = (m_pos / kBlockSize) * kBlockSize;
        int index = findBlock(blockOffset);
        if (index < 0)
            index = fetchBlock(blockOffset);
        if (index < 0)
            return -1;
        Block &block = m_blocks[index];
        block.stamp = ++m_clock;
        const qint64 inBlock = m_pos - block.offset;
        if (inBlock >= block.data.size())
            return 0; // the block came up short: end of file
        // A read spanning the block boundary is answered short, which stream_cb
        // explicitly permits; mpv just asks again.
        const qint64 give = qMin<qint64>(maxSize, block.data.size() - inBlock);
        std::memcpy(buffer, block.data.constData() + inBlock, static_cast<size_t>(give));
        m_pos += give;
        return give;
    }

    if (!positionTo(m_pos))
        return -1;
    const qint64 n = m_provider->read(static_cast<FileHandle *>(m_handle), buffer, maxSize);
    if (n < 0)
        return -1;
    m_handlePos += n;
    m_pos += n;
    m_fetched += n;
    return n;
}

qint64 Stream::seek(qint64 offset) {
    if (offset < 0)
        return -1;
    m_pos = offset;
    return offset;
}

qint64 Stream::size() {
    if (m_sizeKnown)
        return m_size;
    // Opening a handle costs nothing extra here: the read that follows needs
    // one anyway, and on the backends whose transfer is lazy (WebDAV, FTP) no
    // request goes out until the first read.
    if (!m_handle) {
        m_handle = m_provider->openRead(m_path);
        if (!m_handle)
            return -1;
        ++m_opens;
        m_handlePos = 0;
    }
    m_size = m_provider->handleSize(static_cast<FileHandle *>(m_handle));
    m_sizeKnown = true;
    return m_size;
}

void Stream::cancel() {
    m_cancelled.store(true);
}

} // namespace MpvStreamSource
