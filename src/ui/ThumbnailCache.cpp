#include "ThumbnailCache.h"

#include "ExifThumbnail.h"
#include "Mp4RangePlan.h"
#include "VideoRangePlan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThreadPool>

#include <limits>

namespace {

constexpr int kMemCacheBudget = 256;  // entries; QCache cost is 1 per pixmap
constexpr int kMaxWorkerThreads = 4;  // bounded so a folder full of videos
                                       // can't starve the rest of the app

// Extensions ffmpeg is asked to pull a frame from.
const QSet<QString> &videoExtensions() {
    static const QSet<QString> exts = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"),  QStringLiteral("mov"),
        QStringLiteral("avi"),  QStringLiteral("webm"), QStringLiteral("wmv"),
        QStringLiteral("flv"),  QStringLiteral("m4v"),  QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("3gp"),  QStringLiteral("ts"),
    };
    return exts;
}

// Extensions decoded directly via QImageReader. A fixed allowlist rather
// than QImageReader::supportedImageFormats() so canThumbnail() stays a pure
// extension check (no plugin-loading I/O on every call).
const QSet<QString> &imageExtensions() {
    static const QSet<QString> exts = {
        QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"),  QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("ico"),
        QStringLiteral("xpm"),  QStringLiteral("pbm"), QStringLiteral("pgm"),
        QStringLiteral("ppm"),
    };
    return exts;
}

QString extensionOf(const QString &path) { return QFileInfo(path).suffix().toLower(); }

// Byte budgets for a *remote* thumbnail, deliberately far below the 100 MB
// QuickView allows: this runs unattended while the user scrolls, so it must
// cost pennies per file rather than be merely bounded.
//
// An image has no safe partial decode: a truncated JPEG does NOT fail to
// decode -- Qt returns a non-null image with no error set, mostly flat grey --
// so a prefix must never be passed off as a thumbnail. Ordinary images are
// therefore fetched whole, and the budget doubles as a "don't bother"
// threshold. The exception is a camera JPEG, which carries a complete EXIF
// preview in its first few tens of KB: that is tried first (kExifProbeBytes),
// and when it hits, a 20 MB photo costs ~64 KB instead. Because that path
// exists, the whole-file ceiling can stay modest without giving up on big
// photos.
//
// A video is different again: ffmpeg needs the container's index plus a little
// media data to pull an early frame, and only a fraction of the file is ever
// fetched however large it is. For MP4/MOV the exact ranges are computed from
// the container itself (Mp4RangePlan); kRemoteVideoHalf is the fallback for
// formats that cannot be read that way -- both ends of the file, since the
// index sits at one or the other.
constexpr qint64 kRemoteImageBudget = 8LL * 1024 * 1024;
constexpr qint64 kRemoteVideoHalf = 2LL * 1024 * 1024;
// Enough to cover the EXIF APP1 segment of every camera JPEG sampled (their
// previews ended by ~23 KB), with headroom for larger embedded previews.
constexpr qint64 kExifProbeBytes = 128 * 1024;
// Head read used to walk an MP4's top-level box chain. The chain is a handful
// of small boxes before the media, so this is generous; it also covers the
// whole index outright for a short clip.
constexpr qint64 kMp4HeadProbe = 64 * 1024;

bool isVideoPath(const QString &path) { return videoExtensions().contains(extensionOf(path)); }

// JPEGs are the only format that reliably embeds an EXIF preview, so they get
// the cheap header probe; other formats go straight to the whole-file path.
bool isJpegPath(const QString &path) {
    const QString ext = extensionOf(path);
    return ext == QStringLiteral("jpg") || ext == QStringLiteral("jpeg");
}

// Largest remote file still worth a thumbnail attempt, by kind. Videos have no
// size ceiling because only a fixed excerpt of one is ever fetched; JPEGs have
// none either, because the EXIF probe is tried first and a photo too big to
// fetch whole almost always carries one (every camera JPEG sampled did).
qint64 remoteSizeLimit(const QString &path) {
    return (isVideoPath(path) || isJpegPath(path)) ? std::numeric_limits<qint64>::max()
                                                   : kRemoteImageBudget;
}

// Decodes an image file, scaling it to fit within size x size (preserving
// aspect ratio, never upscaling past the source). Returns a null QImage on
// failure. Shared by the direct image path and the ffmpeg-extracted video
// frame -- both are just image files by the time this runs.
QImage decodeScaled(const QString &imagePath, int size) {
    QImageReader reader(imagePath);
    reader.setAutoTransform(true); // honour EXIF orientation

    const QSize original = reader.size();
    if (original.isValid() && !original.isEmpty()) {
        const QSize target = original.scaled(size, size, Qt::KeepAspectRatio);
        if (target.width() < original.width() || target.height() < original.height())
            reader.setScaledSize(target);
    }

    QImage image = reader.read();
    if (image.isNull())
        return {};

    // Belt-and-braces: some format plugins ignore setScaledSize(), or
    // reader.size() was unavailable up front. Scale down again if needed --
    // this never upscales since the check is width/height > size.
    if (image.width() > size || image.height() > size)
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// Parses "Duration: HH:MM:SS.xx" out of ffmpeg's banner text. Returns -1 if
// not found.
double parseDurationSeconds(const QString &text) {
    static const QRegularExpression re(
        QStringLiteral("Duration:\\s*(\\d+):(\\d+):(\\d+(?:\\.\\d+)?)"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return -1.0;
    const double hours = m.captured(1).toDouble();
    const double minutes = m.captured(2).toDouble();
    const double seconds = m.captured(3).toDouble();
    return hours * 3600.0 + minutes * 60.0 + seconds;
}

// Best-effort video duration in seconds, used to pick a "~10% in" seek
// point. Tries ffprobe first (structured, single-value output); falls back
// to parsing ffmpeg's own metadata banner if ffprobe isn't installed.
// Returns -1 if both fail -- the caller then falls back to a fixed offset.
double videoDurationSeconds(const QString &path) {
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (!ffprobe.isEmpty()) {
        QProcess proc;
        proc.start(ffprobe,
                   {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                    QStringLiteral("format=duration"), QStringLiteral("-of"),
                    QStringLiteral("default=noprint_wrappers=1:nokey=1"), path});
        if (proc.waitForFinished(5000) && proc.exitStatus() == QProcess::NormalExit) {
            bool ok = false;
            const double seconds =
                QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toDouble(&ok);
            if (ok && seconds > 0.0)
                return seconds;
        }
    }

    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"), {QStringLiteral("-i"), path});
    proc.waitForFinished(5000);
    // With no output target ffmpeg always exits non-zero, but it still
    // prints the input's metadata (including Duration) to stderr first.
    return parseDurationSeconds(QString::fromUtf8(proc.readAllStandardError()));
}

// Extracts one representative frame (~10% into the video) to a temporary
// PNG file via the system ffmpeg binary. Returns the temp file path (caller
// must remove it) or an empty string on any failure.
QString extractVideoFrame(const QString &path) {
    QTemporaryFile temp(QDir::tempPath() + QStringLiteral("/ttc-thumb-XXXXXX.png"));
    temp.setAutoRemove(false);
    if (!temp.open())
        return {};
    const QString framePath = temp.fileName();
    temp.close();

    const double duration = videoDurationSeconds(path);
    const double seekSeconds = duration > 0.0 ? duration * 0.10 : 1.0;

    QProcess proc;
    const QStringList args = {
        QStringLiteral("-y"),
        QStringLiteral("-ss"),        QString::number(seekSeconds, 'f', 2),
        QStringLiteral("-i"),         path,
        QStringLiteral("-frames:v"),  QStringLiteral("1"),
        QStringLiteral("-q:v"),       QStringLiteral("4"),
        framePath,
    };
    proc.start(QStringLiteral("ffmpeg"), args);
    const bool ok = proc.waitForFinished(15000) && proc.exitStatus() == QProcess::NormalExit
                     && proc.exitCode() == 0 && QFileInfo::exists(framePath)
                     && QFileInfo(framePath).size() > 0;
    if (!ok) {
        QFile::remove(framePath);
        return {};
    }
    return framePath;
}

} // namespace

ThumbnailCache &ThumbnailCache::instance() {
    // Deliberately leaked (never destroyed): this singleton owns a
    // QThreadPool, and background workers may still be generating a
    // thumbnail when the app quits. A function-local `static ThumbnailCache
    // cache;` would run ~ThumbnailCache() -- which blocks in ~QThreadPool()
    // waiting for those workers -- during the static-destruction phase,
    // i.e. after QApplication has already been torn down; touching Qt's
    // threading internals at that point segfaults. Leaking sidesteps the
    // whole shutdown-order hazard: the process reclaims everything on exit.
    static ThumbnailCache *cache = new ThumbnailCache();
    return *cache;
}

ThumbnailCache::ThumbnailCache(QObject *parent)
    : QObject(parent), m_memCache(kMemCacheBudget), m_pool(new QThreadPool(this)) {
    m_pool->setMaxThreadCount(kMaxWorkerThreads);
}

bool ThumbnailCache::canThumbnail(const QString &path) {
    const QString ext = extensionOf(path);
    return imageExtensions().contains(ext) || videoExtensions().contains(ext);
}

QString ThumbnailCache::cacheKey(const QString &absolutePath, qint64 mtimeEpoch, int size) {
    const QByteArray payload = (absolutePath + QLatin1Char('\n') + QString::number(mtimeEpoch)
                                 + QLatin1Char('\n') + QString::number(size))
                                    .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Md5).toHex());
}

QString ThumbnailCache::remoteCacheKey(const QString &connectionId, const QString &path,
                                       qint64 mtimeEpoch, qint64 fileSize, int size) {
    const QByteArray payload = (QStringLiteral("remote:") + connectionId + QLatin1Char('\n') + path
                                 + QLatin1Char('\n') + QString::number(mtimeEpoch)
                                 + QLatin1Char('\n') + QString::number(fileSize)
                                 + QLatin1Char('\n') + QString::number(size))
                                    .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Md5).toHex());
}

QString ThumbnailCache::diskCachePath(const QString &key) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                         + QStringLiteral("/ttc/thumbnails");
    return dir + QLatin1Char('/') + key + QStringLiteral(".png");
}

QPixmap ThumbnailCache::lookupCached(const QString &key) {
    {
        QMutexLocker locker(&m_mutex);
        if (QPixmap *cached = m_memCache.object(key))
            return *cached;
    }

    // Not in memory -- check disk before (re)generating. A hit here is
    // still cheap enough to do on the GUI thread (one small PNG decode).
    const QString diskPath = diskCachePath(key);
    if (QFileInfo::exists(diskPath)) {
        QPixmap pixmap(diskPath);
        if (!pixmap.isNull()) {
            QMutexLocker locker(&m_mutex);
            m_memCache.insert(key, new QPixmap(pixmap));
            return pixmap;
        }
        // Truncated/corrupt cache file (e.g. from a killed process) --
        // remove it so we don't keep tripping over it.
        QFile::remove(diskPath);
    }
    return {};
}

bool ThumbnailCache::claimPending(const QString &key) {
    QMutexLocker locker(&m_mutex);
    if (m_pending.contains(key))
        return false; // already generating; thumbnailReady() will follow
    m_pending.insert(key);
    return true;
}

void ThumbnailCache::releasePending(const QString &key) {
    QMutexLocker locker(&m_mutex);
    m_pending.remove(key);
}

QPixmap ThumbnailCache::thumbnail(const QString &path, int size) {
    if (path.isEmpty() || size <= 0 || !canThumbnail(path))
        return {};

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return {};

    const QString absolutePath = info.absoluteFilePath();
    const qint64 mtimeEpoch = info.lastModified().toSecsSinceEpoch();
    const QString key = cacheKey(absolutePath, mtimeEpoch, size);

    if (QPixmap cached = lookupCached(key); !cached.isNull())
        return cached;
    if (!claimPending(key))
        return {};

    // The singleton outlives every worker: QThreadPool's destructor blocks
    // until all queued/running work finishes, and that destructor runs as
    // part of tearing down `this` (m_pool is a QObject child), so capturing
    // the raw `this` here is safe for the object's whole lifetime.
    QThreadPool *pool = m_pool;
    ThumbnailCache *self = this;
    pool->start(QRunnable::create([self, path, key, size]() { self->generate(path, key, size); }));

    return {};
}

QPixmap ThumbnailCache::remoteThumbnail(const std::shared_ptr<FileProvider> &provider,
                                        const QString &connectionId, const QString &path,
                                        qint64 mtimeEpoch, qint64 fileSize, int size) {
    if (!provider || path.isEmpty() || size <= 0 || !canThumbnail(path))
        return {};
    // Nothing to decode from an empty file, and a file past the budget isn't
    // worth the bytes -- in both cases the delegate keeps the generic icon.
    if (fileSize <= 0 || fileSize > remoteSizeLimit(path))
        return {};

    const QString key = remoteCacheKey(connectionId, path, mtimeEpoch, fileSize, size);
    if (QPixmap cached = lookupCached(key); !cached.isNull())
        return cached;
    if (!claimPending(key))
        return {};

    ThumbnailCache *self = this;
    const bool queued = m_remote.submit(
        provider, [self, path, key, fileSize, size](const RemoteThumbnailFetcher::Ticket &ticket) {
            self->generateRemote(ticket, path, key, fileSize, size);
        });
    if (!queued) {
        // Refused (backlog full, or the backend can't stream). Un-claim the key
        // so the next repaint -- by which time a slot may have freed up -- can
        // ask again; leaving it claimed would strand this file permanently.
        releasePending(key);
    }
    return {};
}

void ThumbnailCache::cancelRemote(const FileProvider *provider) { m_remote.cancel(provider); }

void ThumbnailCache::generate(const QString &path, const QString &key, int size) {
    QImage image;
    if (isVideoPath(path)) {
        const QString framePath = extractVideoFrame(path);
        if (!framePath.isEmpty()) {
            image = decodeScaled(framePath, size);
            QFile::remove(framePath);
        }
    } else {
        image = decodeScaled(path, size);
    }

    if (!image.isNull()) {
        // Persist to disk so future sessions -- and any other request that
        // happens to land on this exact key -- skip regeneration entirely.
        const QString diskPath = diskCachePath(key);
        QDir().mkpath(QFileInfo(diskPath).absolutePath());
        if (!image.save(diskPath, "PNG"))
            QFile::remove(diskPath); // non-fatal: caller still gets the image below
    }

    // QPixmap must only be created/used on the GUI thread on some platform
    // backends, so hand back a QImage and let storeResult() (queued onto
    // the GUI thread) do the QPixmap conversion.
    QMetaObject::invokeMethod(this, "storeResult", Qt::QueuedConnection, Q_ARG(QString, path),
                               Q_ARG(QString, key), Q_ARG(QImage, image));
}

QString ThumbnailCache::fetchVideoExcerpt(const RemoteThumbnailFetcher::Ticket &ticket,
                                          const QString &path, qint64 fileSize) {
    // Ask the container where its index is instead of guessing. One small read
    // of the head names every top-level box; for a file whose index trails the
    // media that yields the index's offset, and a second tiny read gives its
    // size. Only then is the fetch sized correctly -- which matters in both
    // directions: a long film's index can exceed a fixed head budget (leaving
    // it with no thumbnail at all), while a short clip needs a fraction of one.
    const QByteArray head = ticket.readHead(path, kMp4HeadProbe);
    if (!head.isEmpty()) {
        // The extension is not evidence of the format -- on a real share, a
        // whole directory of ".mkv" files turned out to be MPEG-TS -- so the
        // container is identified from these bytes, which are already in hand.
        const VideoRangePlan::Container kind = VideoRangePlan::detect(head);

        if (kind == VideoRangePlan::Container::Iso) {
            Mp4RangePlan::Plan plan = Mp4RangePlan::plan(head, fileSize);
            if (plan.needsProbe) {
                const QByteArray probe =
                    ticket.readRange(path, plan.probeOffset, plan.probeLength);
                plan = Mp4RangePlan::refine(plan, probe, fileSize);
            }
            if (!plan.ranges.isEmpty()) {
                const QString excerpt = ticket.downloadRanges(path, fileSize, plan.ranges);
                if (!excerpt.isEmpty())
                    return excerpt;
            }
        } else {
            // Everything else: headers, the frames the grab seeks to, and a
            // tail for the formats that index there. No per-format parsing --
            // measured on real files, that cost more code and decoded fewer
            // of them than these three ranges do.
            const QVector<VideoRangePlan::Range> ranges =
                VideoRangePlan::plan(kind, fileSize);
            if (!ranges.isEmpty()) {
                const QString excerpt = ticket.downloadRanges(path, fileSize, ranges);
                if (!excerpt.isEmpty())
                    return excerpt;
            }
        }
    }

    // An unreadable head, an unrecognised container, or a plan whose ranges
    // did not come back: fall back to the fixed both-ends excerpt, which needs
    // no knowledge of the format.
    return ticket.downloadHeadAndTail(path, fileSize, kRemoteVideoHalf);
}

void ThumbnailCache::generateRemote(const RemoteThumbnailFetcher::Ticket &ticket,
                                    const QString &path, const QString &key, qint64 fileSize,
                                    int size) {
    QImage image;
    // Cheapest path first: a camera JPEG carries a complete preview in its
    // header, so a big photo can be thumbnailed from a fraction of its bytes.
    // Only worth the extra round trip when fetching the whole file would
    // actually hurt -- a small JPEG is cheaper to just pull.
    if (isJpegPath(path) && fileSize > kExifProbeBytes * 2) {
        const QByteArray preview = ExifThumbnail::extract(ticket.readHead(path, kExifProbeBytes));
        if (!preview.isEmpty()) {
            QImage embedded;
            if (embedded.loadFromData(preview, "JPEG") && !embedded.isNull()) {
                image = embedded.width() > size || embedded.height() > size
                            ? embedded.scaled(size, size, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation)
                            : embedded;
            }
        }
    }

    // A JPEG that got past remoteSizeLimit() on the strength of the EXIF probe
    // but had no usable preview must not now be pulled down in full: fall back
    // to the ordinary whole-file budget, and give up beyond it.
    const bool tooBigToFetchWhole = !isVideoPath(path) && fileSize > kRemoteImageBudget;
    QString localCopy;
    if (image.isNull() && !tooBigToFetchWhole) {
        localCopy = isVideoPath(path) ? fetchVideoExcerpt(ticket, path, fileSize)
                                      : ticket.download(path, fileSize);
    }
    if (!localCopy.isEmpty()) {
        // From here on the fetched excerpt is an ordinary local file, so the
        // decode is byte-for-byte the same work generate() does -- including
        // ffmpeg's frame grab, which simply fails (empty frame path) on the rare
        // container whose index sits at neither end.
        if (isVideoPath(path)) {
            const QString framePath = extractVideoFrame(localCopy);
            if (!framePath.isEmpty()) {
                image = decodeScaled(framePath, size);
                QFile::remove(framePath);
            }
        } else {
            image = decodeScaled(localCopy, size);
        }
        QFile::remove(localCopy); // the temp file has served its purpose
    }

    if (!image.isNull() && !ticket.cancelled()) {
        const QString diskPath = diskCachePath(key);
        QDir().mkpath(QFileInfo(diskPath).absolutePath());
        if (!image.save(diskPath, "PNG"))
            QFile::remove(diskPath); // non-fatal: the in-memory result still lands
    }

    // Reported even when cancelled or empty: storeResult clears the pending
    // claim, and leaving it set would block every later attempt at this file.
    QMetaObject::invokeMethod(this, "storeResult", Qt::QueuedConnection, Q_ARG(QString, path),
                               Q_ARG(QString, key), Q_ARG(QImage, image));
}

void ThumbnailCache::storeResult(const QString &path, const QString &key, QImage image) {
    if (!image.isNull()) {
        QMutexLocker locker(&m_mutex);
        m_pending.remove(key);
        m_memCache.insert(key, new QPixmap(QPixmap::fromImage(image)));
    } else {
        QMutexLocker locker(&m_mutex);
        m_pending.remove(key);
    }
    emit thumbnailReady(path);
}
