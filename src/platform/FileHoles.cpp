#include "FileHoles.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace fc {

namespace {

constexpr int kSampleBytes = 4096;
constexpr int kSweepSamples = 64;
// Around the offset of interest, look a little either side: media is
// interleaved, so the exact byte a player wants is not the only one it needs.
constexpr int kNearbySamples = 5;
constexpr qint64 kNearbySpread = 4 * 1024 * 1024;

bool readsAsZeros(QFile *file, qint64 offset) {
    if (offset < 0 || offset >= file->size())
        return false;
    if (!file->seek(offset))
        return false;
    const QByteArray chunk = file->read(kSampleBytes);
    if (chunk.isEmpty())
        return false;
    return std::all_of(chunk.cbegin(), chunk.cend(), [](char byte) { return byte == '\0'; });
}

} // namespace

qint64 approximateOffsetForTime(qint64 fileSize, double seconds, double durationSeconds) {
    if (fileSize <= 0 || durationSeconds <= 0.0 || seconds < 0.0)
        return -1;
    const double fraction = std::min(seconds / durationSeconds, 1.0);
    const qint64 offset = static_cast<qint64>(fraction * static_cast<double>(fileSize));
    return qBound<qint64>(0, offset, fileSize - 1);
}

HoleReport inspectForHoles(const QString &path, qint64 offset) {
    HoleReport report;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0)
        return report;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return report;
    report.sampled = true;
    const qint64 size = file.size();

    if (offset >= 0 && offset < size) {
        int zeros = 0;
        for (int i = 0; i < kNearbySamples; ++i) {
            const qint64 at =
                offset + (i - kNearbySamples / 2) * (kNearbySpread / kNearbySamples);
            if (at < 0 || at >= size)
                continue;
            if (readsAsZeros(&file, at))
                ++zeros;
        }
        // Every probe that landed in the file came back empty.
        report.aroundOffset = zeros >= kNearbySamples - 1;
    }

    for (int i = 0; i < kSweepSamples; ++i) {
        // Skip the very first sample: a container's header is always written,
        // even in a download that has produced nothing else, so counting it
        // would understate how much is missing.
        const qint64 at = size * (i + 1) / (kSweepSamples + 1);
        ++report.totalSamples;
        if (readsAsZeros(&file, at))
            ++report.zeroSamples;
    }
    return report;
}

} // namespace fc
