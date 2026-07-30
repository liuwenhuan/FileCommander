#include "ThumbnailCache.h"

#include "theme/Phosphor.h"

#include "config/Settings.h"

#include "ExifThumbnail.h"
#include "FileInfo.h"
#include "FileProvider.h" // canStream(): a backend that can't stream is never retried
#include "Mp4RangePlan.h"
#include "VideoRangePlan.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kMaxWorkerThreads = 4;  // bounded so a folder full of videos
                                       // can't starve the rest of the app

// The rungs storageSize() quantises onto. Kept here, in one place, because the
// cache format stamp is derived from this list: editing the ladder changes
// which file every request looks for, so it must also invalidate what is
// already stored -- and deriving the stamp is what makes that automatic rather
// than something a future edit has to remember.
constexpr int kRungLadder[] = {96, 192, 384, 768};

// Bumped BY HAND whenever the bytes hashed into cacheKey()/remoteCacheKey()
// change -- a new field, a different separator, a different hash. Those changes
// are invisible to the ladder above, so nothing else can notice them.
//
// Getting this wrong is not a correctness bug (a key change simply makes every
// stored file unreachable) but it is a silent, permanent leak: unreachable
// entries are never looked up, so nothing ever deletes them. That has already
// happened once for real -- ~98% of a 54 MB cache went unreachable in a single
// commit and had to be removed by hand.
constexpr int kKeyFormatVersion = 1;

// Fraction of the limit pruneToLimit() deletes down to. See the header for why
// the gap has to exist; 80% of 512 MB leaves ~100 MB of headroom, which is
// hundreds of thumbnails at the largest rung -- enough that a sweep lasts.
constexpr int kPruneFloorPercent = 80;

// How stale an entry's mtime must be before a cache hit rewrites it.
//
// mtime is doing double duty here: it is both "when this was written" and "when
// this was last used", which is what pruneToLimit() sorts on. Refreshing it on
// every hit would be correct and unaffordable -- one screenful of an icon grid
// is dozens of hits, and scrolling turns that into a metadata write per file
// per frame. A day's resolution is far finer than the prune needs (it only has
// to order entries well enough to pick the coldest) and costs at most one write
// per file per day.
//
// atime would carry the same information for free, and is not usable: this
// machine mounts /home with relatime, where atime is only updated if it is
// already a day old or predates mtime, and a noatime mount never updates it at
// all. An mtime we write ourselves is the only stamp that behaves the same
// everywhere.
constexpr qint64 kTouchIntervalSecs = 24 * 60 * 60;

// Identifies the on-disk layout: a stored file is only meaningful to a build
// that agrees with both halves. Written to `<cacheDir>/version`.
QString cacheFormatStamp() {
    QStringList rungs;
    for (const int rung : kRungLadder)
        rungs << QString::number(rung);
    return QStringLiteral("keys=%1 rungs=%2")
        .arg(kKeyFormatVersion)
        .arg(rungs.join(QLatin1Char(',')));
}

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

QString extensionOf(const QString &path) {
    return FileInfo::suffixForName(QFileInfo(path).fileName()).toLower();
}

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

// Writes `image` to the disk cache atomically: QSaveFile stages the bytes in a
// sibling temp file and renames it into place on commit(), so `diskPath` never
// exists in a half-written state.
//
// That matters because a worker's write races the GUI thread's lookup of the
// very same key: two zoom steps on one rung read the file the other is still
// writing, and lookupCached() treats a QImage it cannot decode as a corrupt
// leftover and DELETES it. A plain save() therefore loses a perfectly good
// thumbnail every so often and regenerates it -- exactly the cost this cache
// exists to avoid. Renaming is atomic, so the file is either absent or whole.
bool saveThumbnail(const QImage &image, const QString &diskPath) {
    QDir().mkpath(QFileInfo(diskPath).absolutePath());
    QSaveFile file(diskPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (!image.save(&file, "PNG")) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

// Marks a cache file as used, so pruneToLimit() sees it as warm -- but only
// once its stamp is already a day old (see kTouchIntervalSecs). `modified` is
// the mtime the caller has just stat'd, passed in so this costs no extra stat
// on the common path, where it does nothing at all.
//
// Failure is ignored on purpose: a read-only cache directory, or a file another
// process removed between the stat and here, costs at worst a slightly-too-cold
// entry. Nothing about a thumbnail lookup should fail over bookkeeping.
void touchIfStale(const QString &diskPath, const QDateTime &modified) {
    const QDateTime now = QDateTime::currentDateTime();
    if (modified.isValid() && modified.secsTo(now) < kTouchIntervalSecs)
        return;
    // Windows requires a writable handle for SetFileTime. The cache is owned by
    // this process, so opening read/write is safe and remains non-destructive.
    QFile file(diskPath);
    if (file.open(QIODevice::ReadWrite))
        file.setFileTime(now, QFileDevice::FileModificationTime);
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

// Decoder complaints that mean the bytes ffmpeg was given are not the bytes it
// needed. A remote thumbnail is taken from a sparse excerpt, so when the seek
// lands in a hole the decoder does NOT fail -- it reports these and emits a
// flat grey frame, which then exits 0 and looks like a success. Matching the
// message is what separates "we fetched the wrong bytes" from "this frame is
// legitimately a single colour", which no pixel statistic can tell apart.
bool hasDecodeErrors(const QString &stderrText) {
    static const QStringList markers = {
        QStringLiteral("Invalid NAL"),          QStringLiteral("Error splitting"),
        QStringLiteral("error while decoding"), QStringLiteral("Invalid data found"),
        QStringLiteral("non-existing PPS"),     QStringLiteral("decode_slice_header"),
        QStringLiteral("could not find ref"),   QStringLiteral("concealing"),
    };
    for (const QString &marker : markers) {
        if (stderrText.contains(marker, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Grey-screen threshold, in greyscale standard deviation. Sits in the gap
// measured between a frame decoded from a hole (3.1) and one decoded from
// partially-damaged but present bytes (68.5); real content sits far above it.
// Only ever consulted for a frame ffmpeg already complained about, so a
// legitimately flat picture -- black open, white flash, solid transition, all
// of which measure 0.0 -- never reaches it.
constexpr double kFlatFrameStdDev = 10.0;

// Greyscale standard deviation of `image`, the "is there anything here" test.
// Returns -1 for an image that cannot be read.
double frameStdDev(const QString &imagePath) {
    QImage image(imagePath);
    if (image.isNull())
        return -1.0;
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    quint64 count = 0;
    quint64 sum = 0;
    quint64 sumSquares = 0;
    for (int y = 0; y < grey.height(); ++y) {
        const uchar *line = grey.constScanLine(y);
        for (int x = 0; x < grey.width(); ++x) {
            const quint64 v = line[x];
            sum += v;
            sumSquares += v * v;
            ++count;
        }
    }
    if (count == 0)
        return -1.0;
    const double mean = double(sum) / double(count);
    const double variance = double(sumSquares) / double(count) - mean * mean;
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

// Extracts one representative frame to a temporary PNG file via the system
// ffmpeg binary. Returns the temp file path (caller must remove it) or an empty
// string on any failure.
//
// `seekOverride` names the exact second to seek to. A remote excerpt passes the
// timestamp of the keyframe it actually fetched, so the first frame decoded is
// the one already on disk; without it the grab probes the duration and seeks to
// 10%, which for a sparse excerpt generally lands between the fetched
// keyframes -- and a decoder cannot reach a later point when the frames leading
// to it were never pulled. Negative means "work it out from the duration".
QString extractVideoFrame(const QString &path, double seekOverride = -1.0) {
    QTemporaryFile temp(QDir::tempPath() + QStringLiteral("/FileCommander-thumb-XXXXXX.png"));
    temp.setAutoRemove(false);
    if (!temp.open())
        return {};
    const QString framePath = temp.fileName();
    temp.close();

    double seekSeconds = seekOverride;
    if (seekSeconds < 0.0) {
        const double duration = videoDurationSeconds(path);
        seekSeconds = duration > 0.0 ? duration * 0.10 : 1.0;
    }

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
    // Exiting 0 with a non-empty PNG is not enough. Fed a sparse excerpt whose
    // seek point was never fetched, ffmpeg reports the damage on stderr but
    // still writes a flat grey frame and exits 0 -- which the checks above
    // accept, and which then gets cached as this file's thumbnail forever.
    //
    // Both signals are required, because either alone is wrong. Flatness alone
    // rejects real content: a legitimately black, white or single-colour frame
    // measures 0.0, i.e. FLATTER than the grey one at 3.1. Decoder complaints
    // alone reject good frames too: bytes damaged away from the seek point
    // produce warnings and a perfectly usable picture (68.5). Only a frame that
    // both drew complaints and carries no detail is the failure being caught.
    const bool complained =
        ok && hasDecodeErrors(QString::fromUtf8(proc.readAllStandardError()));
    const bool damaged = complained && frameStdDev(framePath) < kFlatFrameStdDev;
    if (!ok || damaged) {
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
    : QObject(parent), m_memCache(kMemoryBudgetKiB), m_pool(new QThreadPool(this)) {
    m_pool->setMaxThreadCount(kMaxWorkerThreads);
    // Before the first thumbnail of the session is written, never after: see
    // purgeIfStale()'s declaration. Costs one small file read when the stamp
    // matches, which is every launch but the one following a format change.
    purgeIfStale();
}

int ThumbnailCache::pixmapCostKiB(const QPixmap &pixmap) {
    const qint64 bits = qint64(qMax(0, pixmap.width())) * qMax(0, pixmap.height())
                         * qMax(0, pixmap.depth());
    const qint64 bytes = (bits + 7) / 8;
    const qint64 kib = qMax<qint64>(1, (bytes + 1023) / 1024);
    return int(qMin<qint64>(kib, std::numeric_limits<int>::max()));
}

void ThumbnailCache::setMemoryBudgetKiBForTest(int budgetKiB) {
    QMutexLocker locker(&m_mutex);
    m_memCache.clear();
    m_memCache.setMaxCost(qMax(1, budgetKiB));
}

void ThumbnailCache::insertPixmapForTest(const QString &key, const QPixmap &pixmap) {
    QMutexLocker locker(&m_mutex);
    insertPixmap(key, pixmap);
}

ThumbnailCache::ThumbnailMemoryStats ThumbnailCache::memoryStatsForTest() {
    QMutexLocker locker(&m_mutex);
    return {m_memCache.size(), qint64(m_memCache.totalCost()) * 1024};
}

bool ThumbnailCache::canThumbnail(const QString &path) {
    const QString ext = extensionOf(path);
    return imageExtensions().contains(ext) || videoExtensions().contains(ext);
}

int ThumbnailCache::storageSize(int requested) {
    // A doubling ladder, deliberately coarse. The reuse it buys is what the
    // zoom steps feel, and the oversampling it costs is close to free: at most
    // 4x the stored pixels of a thumbnail that is a few tens of KB either way,
    // and no extra generation time at all (the fetch and the frame grab do not
    // depend on the target size). Finer rungs would trade that cheap disk back
    // for the expensive thing -- regenerating.
    //
    // 768 covers the largest icon (192 logical) at a 4x display; past the
    // ladder the exact size is stored, so an unforeseen scale still works, it
    // just stops sharing.
    for (const int rung : kRungLadder) {
        if (requested <= rung)
            return rung;
    }
    return requested;
}

QImage ThumbnailCache::scaledForDisplay(const QImage &image, int displaySize) {
    if (image.isNull() || displaySize <= 0)
        return image;
    // Only ever down. A stored bitmap is >= the display size by construction
    // (storageSize rounds up), but a small source file can leave it smaller --
    // decodeScaled refuses to upscale -- and that case must pass straight
    // through rather than being blown up to fill the box.
    if (image.width() <= displaySize && image.height() <= displaySize)
        return image;
    return image.scaled(displaySize, displaySize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QString ThumbnailCache::contentTintTag() {
    // Appended to the MEMORY key only, never the disk key. The stored PNG stays
    // the untouched original, so switching the phosphor toggle costs nothing on
    // disk -- nothing is invalidated and nothing is regenerated; only the cheap
    // derived form in memory is rebuilt. Putting the tint in the disk key
    // instead would double the on-disk cache and re-run every remote fetch and
    // every ffmpeg frame grab on a theme switch.
    const QColor tint = fc::contentTint();
    return tint.isValid() ? QLatin1Char('#') + tint.name() : QString();
}

QPixmap ThumbnailCache::tintedForDisplay(const QPixmap &pixmap) {
    return fc::tintedPixmap(pixmap, fc::contentTint());
}

void ThumbnailCache::invalidateMemoryCache() {
    QMutexLocker locker(&m_mutex);
    m_memCache.clear();
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

QString ThumbnailCache::cacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/FileCommander/thumbnails");
}

QString ThumbnailCache::diskCachePath(const QString &key) {
    return cacheDirectory() + QLatin1Char('/') + key + QStringLiteral(".png");
}

qint64 ThumbnailCache::diskCacheLimitBytes() {
    return qint64(Settings().thumbnailCacheLimitMb()) * 1024 * 1024;
}

int ThumbnailCache::purgeIfStale() {
    const QString dir = cacheDirectory();
    const QString stampPath = dir + QStringLiteral("/version");
    const QByteArray wanted = cacheFormatStamp().toUtf8();

    QFile stamp(stampPath);
    if (stamp.open(QIODevice::ReadOnly) && stamp.readAll().trimmed() == wanted)
        return 0; // written by a build that agrees with this one
    stamp.close();

    // Either no stamp (a cache from before this existed, or a fresh install) or
    // one from an incompatible build. Everything in there is unreachable, so
    // there is nothing to weigh up -- it all goes. Only *.png, though: those are
    // the files this cache writes, and a wipe has no business deleting anything
    // it does not recognise.
    int removed = 0;
    QDir cache(dir);
    for (const QString &name : cache.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        if (QFile::remove(cache.absoluteFilePath(name)))
            ++removed;
    }

    QDir().mkpath(dir);
    QSaveFile out(stampPath);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(wanted);
        out.commit();
    }
    return removed;
}

int ThumbnailCache::pruneToLimit(qint64 limitBytes) {
    if (limitBytes <= 0)
        return 0;

    struct Entry {
        QString path;
        qint64 size = 0;
        qint64 usedAt = 0; // mtime, seconds since epoch -- see kTouchIntervalSecs
    };

    QVector<Entry> entries;
    qint64 total = 0;
    QDirIterator it(cacheDirectory(), {QStringLiteral("*.png")}, QDir::Files);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        entries.append({info.absoluteFilePath(), info.size(),
                        info.lastModified().toSecsSinceEpoch()});
        total += info.size();
    }
    if (total <= limitBytes)
        return 0;

    // Coldest first. Files written by this very session sort last, so a prune
    // triggered by an oversized cache never eats what the user is looking at.
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.usedAt < b.usedAt; });

    const qint64 floorBytes = limitBytes / 100 * kPruneFloorPercent;
    int removed = 0;
    for (const Entry &entry : entries) {
        if (total <= floorBytes)
            break;
        // A file that would not go (another process took it first, or the
        // directory is not writable) is skipped rather than counted: its bytes
        // are still there, so the sweep has to keep looking for room elsewhere.
        if (!QFile::remove(entry.path))
            continue;
        total -= entry.size;
        ++removed;
    }
    return removed;
}

void ThumbnailCache::scheduleMaintenance(int delayMs) {
    if (m_maintenanceScheduled)
        return;
    m_maintenanceScheduled = true;

    // The limit is read here, on the caller's thread, because QSettings reads a
    // file and the worker exists to keep file work off the startup path.
    const qint64 limit = diskCacheLimitBytes();
    QThreadPool *pool = m_pool;
    QTimer::singleShot(delayMs, this, [pool, limit] {
        pool->start(QRunnable::create([limit] { pruneToLimit(limit); }));
    });
}

QPixmap ThumbnailCache::lookupCached(const QString &memKey, const QString &diskKey,
                                     int displaySize, CacheIntent intent) {
    if (intent == CacheIntent::Display) {
        QMutexLocker locker(&m_mutex);
        if (QPixmap *cached = m_memCache.object(memKey))
            return *cached;
    }

    // Not in memory -- check disk before (re)generating. A hit here is still
    // cheap enough to do on the GUI thread (one small PNG decode plus, when
    // the stored rung is larger than this zoom step, one scale-down). That
    // scale is the entire cost of changing zoom on an already-cached
    // directory, in place of re-fetching and re-decoding every file.
    const QString diskPath = diskCachePath(diskKey);
    const QFileInfo cachedFile(diskPath);
    if (cachedFile.exists()) {
        const QImage stored(diskPath);
        if (!stored.isNull()) {
            // This entry has just proved its worth, so record the use for the
            // prune. Throttled hard -- see touchIfStale -- because this line
            // sits on the scroll path.
            touchIfStale(diskPath, cachedFile.lastModified());
            // Tint the derived copy, not the stored one: the disk bitmap is the
            // expensive original and stays hue-neutral.
            const QPixmap pixmap =
                tintedForDisplay(QPixmap::fromImage(scaledForDisplay(stored, displaySize)));
            if (!pixmap.isNull()) {
                if (intent == CacheIntent::Display) {
                    QMutexLocker locker(&m_mutex);
                    insertPixmap(memKey, pixmap);
                }
                return pixmap;
            }
        }
        // Truncated/corrupt cache file (e.g. from a killed process) --
        // remove it so we don't keep tripping over it.
        QFile::remove(diskPath);
    }
    return {};
}

void ThumbnailCache::insertPixmap(const QString &key, const QPixmap &pixmap) {
    m_memCache.insert(key, new QPixmap(pixmap), pixmapCostKiB(pixmap));
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
    const int stored = storageSize(size);
    const QString memKey = cacheKey(absolutePath, mtimeEpoch, size) + contentTintTag();
    const QString diskKey = cacheKey(absolutePath, mtimeEpoch, stored);

    if (QPixmap cached = lookupCached(memKey, diskKey, size, CacheIntent::Display); !cached.isNull())
        return cached;
    // Claimed on the disk key -- the rung is the unit of work, so two zoom
    // steps sharing one rung must not both generate it. The one that loses the
    // race picks the result up from disk on the repaint thumbnailReady brings.
    if (!claimPending(diskKey))
        return {};

    // The singleton outlives every worker: QThreadPool's destructor blocks
    // until all queued/running work finishes, and that destructor runs as
    // part of tearing down `this` (m_pool is a QObject child), so capturing
    // the raw `this` here is safe for the object's whole lifetime.
    QThreadPool *pool = m_pool;
    ThumbnailCache *self = this;
    pool->start(QRunnable::create([self, path, diskKey, memKey, stored, size]() {
        self->generate(path, diskKey, memKey, stored, size);
    }));

    return {};
}

QPixmap ThumbnailCache::remoteThumbnail(const std::shared_ptr<FileProvider> &provider,
                                        const QString &connectionId, const QString &path,
                                        qint64 mtimeEpoch, qint64 fileSize, int size) {
    QPixmap ready;
    requestRemoteThumbnail(provider, connectionId, path, mtimeEpoch, fileSize, size,
                           &ready, CacheIntent::Display);
    return ready;
}

ThumbnailCache::Request ThumbnailCache::requestRemoteThumbnail(
    const std::shared_ptr<FileProvider> &provider, const QString &connectionId,
    const QString &path, qint64 mtimeEpoch, qint64 fileSize, int size, QPixmap *ready,
    CacheIntent intent) {
    // A backend with no streaming support can never serve a thumbnail, so this
    // is Skipped, not Busy -- retrying it would spin forever.
    if (!provider || !provider->canStream() || path.isEmpty() || size <= 0 || !canThumbnail(path))
        return Request::Skipped;
    // Nothing to decode from an empty file, and a file past the budget isn't
    // worth the bytes -- in both cases the delegate keeps the generic icon.
    if (fileSize <= 0 || fileSize > remoteSizeLimit(path))
        return Request::Skipped;

    const int stored = storageSize(size);
    const QString memKey =
        remoteCacheKey(connectionId, path, mtimeEpoch, fileSize, size) + contentTintTag();
    const QString diskKey = remoteCacheKey(connectionId, path, mtimeEpoch, fileSize, stored);
    if (QPixmap cached = lookupCached(memKey, diskKey, size, intent); !cached.isNull()) {
        if (ready)
            *ready = cached;
        return Request::Ready;
    }
    // Already generating: someone else's request covers this row, and
    // thumbnailReady() will announce it. Nothing left for this caller to do.
    // Claimed on the rung, so a zoom step that shares one never re-fetches.
    if (!claimPending(diskKey))
        return Request::Queued;

    ThumbnailCache *self = this;
    const bool queued =
        m_remote.submit(provider, [self, path, diskKey, memKey, fileSize, stored, size,
                                   intent](const RemoteThumbnailFetcher::Ticket &ticket) {
            self->generateRemote(ticket, path, diskKey, memKey, fileSize, stored, size, intent);
        });
    if (!queued) {
        // Refused (backlog full, or the backend can't stream). Un-claim the key
        // so a later attempt -- the next repaint, or the sweep's next pump --
        // can ask again; leaving it claimed would strand this file permanently.
        releasePending(diskKey);
        return Request::Busy;
    }
    return Request::Queued;
}

void ThumbnailCache::cancelRemote(const FileProvider *provider) { m_remote.cancel(provider); }

void ThumbnailCache::generate(const QString &path, const QString &diskKey, const QString &memKey,
                              int storedSize, int displaySize) {
    // Decoded at the rung, not at the display size: this bitmap is what lands
    // on disk and serves every zoom step that shares the rung.
    QImage image;
    if (isVideoPath(path)) {
        const QString framePath = extractVideoFrame(path);
        if (!framePath.isEmpty()) {
            image = decodeScaled(framePath, storedSize);
            QFile::remove(framePath);
        }
    } else {
        image = decodeScaled(path, storedSize);
    }

    if (!image.isNull()) {
        // Persist to disk so future sessions -- and any other request that
        // happens to land on this exact key -- skip regeneration entirely.
        // Non-fatal on failure: the caller still gets the image below and the
        // next request regenerates; QSaveFile leaves nothing partial behind.
        saveThumbnail(image, diskCachePath(diskKey));
    }

    // QPixmap must only be created/used on the GUI thread on some platform
    // backends, so hand back a QImage and let storeResult() (queued onto
    // the GUI thread) do the QPixmap conversion. The scale-down to the size
    // actually being painted happens here, on the worker, rather than on the
    // GUI thread in storeResult().
    QMetaObject::invokeMethod(this, "storeResult", Qt::QueuedConnection, Q_ARG(QString, path),
                               Q_ARG(QString, diskKey), Q_ARG(QString, memKey),
                               Q_ARG(QImage, scaledForDisplay(image, displaySize)), Q_ARG(bool, true));
}

namespace {

// Fraction of the duration the frame grab seeks to; mirrors extractVideoFrame.
constexpr double kFrameSeekFraction = 0.10;

// Fetches the index the plan already located, and asks it where the keyframe at
// the seek point lives. Returns a zero-length range when the index cannot be
// read or holds no usable video track, leaving the plan as it was.
Mp4RangePlan::Keyframe keyframeRangeFor(const RemoteThumbnailFetcher::Ticket &ticket,
                                        const QString &path, const Mp4RangePlan::Plan &plan,
                                        qint64 fileSize) {
    // Both plan shapes put the index in a different slot, and neither position
    // nor size identifies it on its own:
    //   * index leading  -> [0, indexEnd] then a slice of frame data after it,
    //     so the index is the FIRST range and the second one starts non-zero;
    //   * index trailing -> a slice of frame data from 0 then [indexOffset, len],
    //     so the index is the SECOND range -- and often the smaller of the two,
    //     since a compact index loses to a 256 KB slice of media.
    // Rather than guess, read each candidate and let the parser say: it returns
    // nothing for a buffer that holds no video track, so a wrong pick costs one
    // read and falls through to the right one.
    for (const Mp4RangePlan::Range &r : plan.ranges) {
        if (r.second <= 0)
            continue;
        const QByteArray buf = ticket.readRange(path, r.first, r.second);
        if (buf.isEmpty())
            continue;
        const Mp4RangePlan::Keyframe kf =
            Mp4RangePlan::keyframeAt(buf, r.first, fileSize, kFrameSeekFraction);
        if (kf.valid())
            return kf;
    }
    return {};
}

} // namespace

ThumbnailCache::VideoExcerpt ThumbnailCache::fetchVideoExcerpt(
    const RemoteThumbnailFetcher::Ticket &ticket, const QString &path, qint64 fileSize) {
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
                // The plan so far covers the index and the bytes just past it.
                // That is not where the frame is taken from: the grab seeks to
                // 10% of the duration, which on a long file is hundreds of MB
                // in. Read the index and let it name the exact keyframe there,
                // so one small extra range replaces both a wrong guess and a
                // wastefully wide window.
                const Mp4RangePlan::Keyframe keyframe =
                    keyframeRangeFor(ticket, path, plan, fileSize);
                if (keyframe.valid())
                    plan.ranges.append(keyframe.range);

                const QString excerpt = ticket.downloadRanges(path, fileSize, plan.ranges);
                if (!excerpt.isEmpty()) {
                    // Seek to the keyframe that was actually fetched. The
                    // default 10% seek generally falls somewhere after it, and
                    // reaching that point means decoding every frame in
                    // between -- 4.2 MB on a 44 MB clip whose opening keyframe
                    // runs five seconds, against the 2 MB the window holds. The
                    // frame is no less representative: it is the keyframe
                    // nearest 10%, not the file's first.
                    return {excerpt, keyframe.seconds};
                }
            }
        } else if (kind == VideoRangePlan::Container::MpegTs) {
            // A transport stream is the one container that must NOT be handed a
            // sparse excerpt. It carries no index and no global header: the
            // demuxer syncs by finding 0x47 every 188 bytes, so the holes read
            // as zeroes, sync is never found, and it rescans byte by byte over
            // the file's whole apparent length. Measured on a 1.5 GB share
            // file: 25.6 s sparse against 0.2 s for the very same bytes written
            // contiguously -- and the 15 s grab timeout turns the former into
            // no thumbnail at all.
            //
            // Nothing is lost by dropping the true offsets here, because a
            // stream has no absolute pointers to honour. The duration is
            // unknowable from an excerpt for the same reason (it is derived by
            // scanning PTS), so the grab falls back to a fixed early seek --
            // which is why one run from 10% in is all this needs.
            const VideoRangePlan::Range run = VideoRangePlan::contiguousStreamRange(fileSize);
            const QString excerpt = ticket.downloadContiguous(path, run.first, run.second);
            if (!excerpt.isEmpty())
                return {excerpt, -1.0};
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
                    return {excerpt, -1.0};
            }
        }
    }

    // An unreadable head, an unrecognised container, or a plan whose ranges
    // did not come back: fall back to the fixed both-ends excerpt, which needs
    // no knowledge of the format.
    return {ticket.downloadHeadAndTail(path, fileSize, kRemoteVideoHalf), -1.0};
}

void ThumbnailCache::generateRemote(const RemoteThumbnailFetcher::Ticket &ticket,
                                    const QString &path, const QString &diskKey,
                                    const QString &memKey, qint64 fileSize, int storedSize,
                                    int displaySize, CacheIntent intent) {
    // As in generate(): everything decodes at the rung, and only the copy
    // handed to the memory cache is brought down to the display size. Note
    // that none of the fetching below depends on the size at all -- the byte
    // budgets and range plans are the same either way -- which is why storing
    // a larger rung costs no extra network.
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
                image = embedded.width() > storedSize || embedded.height() > storedSize
                            ? embedded.scaled(storedSize, storedSize, Qt::KeepAspectRatio,
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
    double seekSeconds = -1.0;
    if (image.isNull() && !tooBigToFetchWhole) {
        if (isVideoPath(path)) {
            const VideoExcerpt excerpt = fetchVideoExcerpt(ticket, path, fileSize);
            localCopy = excerpt.path;
            seekSeconds = excerpt.seekSeconds;
        } else {
            localCopy = ticket.download(path, fileSize);
        }
    }
    if (!localCopy.isEmpty()) {
        // From here on the fetched excerpt is an ordinary local file, so the
        // decode is byte-for-byte the same work generate() does -- including
        // ffmpeg's frame grab, which simply fails (empty frame path) on the rare
        // container whose index sits at neither end.
        if (isVideoPath(path)) {
            const QString framePath = extractVideoFrame(localCopy, seekSeconds);
            if (!framePath.isEmpty()) {
                image = decodeScaled(framePath, storedSize);
                QFile::remove(framePath);
            }
        } else {
            image = decodeScaled(localCopy, storedSize);
        }
        QFile::remove(localCopy); // the temp file has served its purpose
    }

    if (!image.isNull() && !ticket.cancelled()) {
        // Non-fatal on failure: the in-memory result still lands and the next
        // request regenerates; QSaveFile leaves nothing partial behind.
        saveThumbnail(image, diskCachePath(diskKey));
    }

    // Reported even when cancelled or empty: storeResult clears the pending
    // claim, and leaving it set would block every later attempt at this file.
    QMetaObject::invokeMethod(this, "storeResult", Qt::QueuedConnection, Q_ARG(QString, path),
                               Q_ARG(QString, diskKey), Q_ARG(QString, memKey),
                               Q_ARG(QImage, scaledForDisplay(image, displaySize)),
                               Q_ARG(bool, intent == CacheIntent::Display));
}

void ThumbnailCache::storeResult(const QString &path, const QString &pendingKey,
                                 const QString &memKey, QImage image, bool cacheForDisplay) {
    if (!image.isNull() && cacheForDisplay) {
        QMutexLocker locker(&m_mutex);
        m_pending.remove(pendingKey);
        insertPixmap(memKey, tintedForDisplay(QPixmap::fromImage(image)));
    } else {
        QMutexLocker locker(&m_mutex);
        m_pending.remove(pendingKey);
    }
    emit thumbnailReady(path);
}
