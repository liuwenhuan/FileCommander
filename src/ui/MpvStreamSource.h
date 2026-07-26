#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <memory>

class FileProvider;
struct mpv_handle;

// Plays a file that lives on a network backend without downloading it first.
//
// The preview pane used to fetch a remote file whole into /tmp before it could
// be shown, which for video meant waiting out the entire transfer (and, past a
// 100 MB cut-off, getting no preview at all). libmpv can instead be handed a
// custom protocol whose read/seek/size callbacks we implement -- so mpv pulls
// only the bytes it actually decodes, straight through the existing
// FileProvider streaming interface.
//
// Measured against a 133 MB / 12-minute H.264 MP4, counting every byte the
// callbacks handed over: 1.97 MB to start playing, and 4.68 MB (3.5% of the
// file) after also seeking to the 90% mark. An MPEG-TS of the same length came
// to 5.4 MB, an MP4 with its moov atom at the tail to 4.8 MB.
//
// Going through FileProvider rather than letting mpv open the URL itself is
// what makes this work at all for SMB: this machine's libmpv answers
// "Protocol not found. Make sure FFmpeg is compiled with networking support."
// for smb:// (its FFmpeg is built without libsmbclient). It also keeps every
// password inside the process -- mpv never sees a credential -- and reuses the
// backend's own connection pooling, timeouts and concurrency limits instead of
// opening a second, unmanaged connection behind their back.
namespace MpvStreamSource {

// The URL scheme registered with libmpv. Callers only need isStreamUrl().
extern const char *const kScheme;

// Registers the protocol handler on `mpv`. Call once per handle, after
// mpv_initialize(). Returns false if libmpv refused the registration.
bool registerProtocol(mpv_handle *mpv);

// Publishes `remotePath` on `provider` and returns the URL to hand to mpv.
// Returns an empty string if the backend cannot stream.
//
// The provider is held by shared_ptr for as long as the URL is published *and*
// for as long as any stream opened from it is still reading, so a tab that
// disconnects mid-playback cannot pull the backend out from under mpv.
//
// Only a small number of the most recent URLs stay published (older ones are
// dropped); a stream that is already open is unaffected, since it holds its own
// reference. Publishing is bounded rather than explicitly revoked because
// loadfile is asynchronous: revoking the URL we just handed over would race
// with mpv getting round to opening it.
QString publish(const std::shared_ptr<FileProvider> &provider, const QString &remotePath);

// Whether `path` is one of our URLs rather than a filesystem path. Callers use
// this to skip the "does this file exist on disk" checks that a preview
// normally does.
bool isStreamUrl(const QString &path);

// Drops a published URL. Exposed for tests; the UI relies on the bound above.
void revoke(const QString &url);

// How many streams are open right now. Each one holds a read channel on its
// backend for as long as it plays, so anything else that sizes a worker pool
// against maxReadChannels() has to subtract these or it will count a channel
// that is already spoken for.
int activeStreams();

// The byte pump behind the protocol: one instance per open stream, driven by
// libmpv's callbacks.
//
// Exposed here so its positioning behaviour can be tested directly, with a
// scripted access pattern and without a libmpv instance. That behaviour is the
// part worth testing, because the backends do not agree on what a seek is:
//
//   SMB  (SmbProvider::seek -> smbc_lseek) and SFTP (libssh2_sftp_seek64)
//        seek an open handle for real, at no cost.
//   WebDAV and FTP only honour a seek *before* their transfer starts, where it
//        turns into an HTTP Range / FTP REST on the request that follows; once
//        bytes have flowed their seek() refuses (CurlWebDavProvider.cpp,
//        CurlFtpProvider.cpp). RemoteThumbnailFetcher hits the same wall and
//        answers it the same way: open a fresh handle per range.
//
// Rather than branch on the backend, Stream just asks: seek() is free to
// attempt (on the refusing backends it is a plain flag test, not a round trip),
// and only when it says no does the stream fall back to skipping forward or
// re-establishing the transfer.
class Stream {
public:
    Stream(std::shared_ptr<FileProvider> provider, QString remotePath);
    ~Stream();

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    // read(2) semantics, as libmpv's stream_cb requires: blocks until it has
    // something, returns 0 only at genuine EOF, -1 on error. Short reads are
    // allowed and mpv simply asks again.
    qint64 read(char *buffer, qint64 maxSize);

    // Records the new position and returns it; the backend is not touched until
    // the next read. Laziness matters: mpv issues a seek to 0 immediately after
    // opening every stream purely to test seekability, and paying for a
    // reconnect there would cost a round trip on every single file.
    qint64 seek(qint64 offset);

    // Total size in bytes, or -1 if the backend won't say.
    qint64 size();

    // Interrupts the stream from another thread (libmpv's cancel callback,
    // which must not block). Reads already blocked inside the backend cannot be
    // torn out of it -- this stops the next one and every one after.
    void cancel();

    // --- Diagnostics / test seam ---
    // Bytes actually pulled off the backend, including any skipped over to
    // avoid a reconnect. This is the number that says whether the file was
    // streamed or downloaded.
    qint64 bytesFetched() const { return m_fetched; }
    // How many times a transfer had to be established. One means the whole
    // session ran on a single connection.
    int opens() const { return m_opens; }

private:
    // Gets the backend positioned at `target`, opening or re-opening as needed.
    bool positionTo(qint64 target);
    // Reads and discards up to `target`, for gaps too small to be worth a
    // reconnect.
    bool skipForward(qint64 target);
    // Pulls the block containing `offset` into the cache; returns its index or
    // -1. Only used for small reads (see read()).
    int fetchBlock(qint64 offset);
    int findBlock(qint64 offset) const;
    void closeHandle();

    std::shared_ptr<FileProvider> m_provider;
    QString m_path;

    // The backend handle, and where it actually sits, versus where the stream
    // logically is. They diverge between a seek and the read that follows it.
    void *m_handle = nullptr; // FileHandle*, opaque here to keep the header light
    qint64 m_handlePos = 0;
    qint64 m_pos = 0;
    qint64 m_size = -1;
    bool m_sizeKnown = false;

    std::atomic<bool> m_cancelled{false};
    qint64 m_fetched = 0;
    int m_opens = 0;

    // A handful of recently read blocks, to collapse the cluster of tiny reads
    // a seek can provoke. Formats without an index -- MPEG-TS above all -- have
    // no way to turn a timestamp into an offset, so the demuxer bisects the
    // file: seeking to the 90% mark of a 141 MB .ts was measured issuing 23
    // seeks, and eight of those probes landed inside the same 18 KB span. On
    // SMB or SFTP that is 23 cheap lseeks; on WebDAV or FTP each one would
    // otherwise be another HTTP request.
    struct Block {
        qint64 offset = -1;
        QByteArray data;
        quint64 stamp = 0;
    };
    static constexpr int kBlockCount = 16;
    Block m_blocks[kBlockCount];
    quint64 m_clock = 0;
};

} // namespace MpvStreamSource
