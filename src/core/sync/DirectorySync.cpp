#include "DirectorySync.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <algorithm>

QVector<SyncEntry> DirectorySync::compare(const QString &leftDir, const QString &rightDir,
                                           bool recursive) {
    const QDir leftBase(leftDir);
    const QDir rightBase(rightDir);
    QMap<QString, QFileInfo> leftFiles, rightFiles;

    const QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden;
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;

    QDirIterator leftIt(leftDir, filters, flags);
    while (leftIt.hasNext()) {
        leftIt.next();
        const QFileInfo fi = leftIt.fileInfo();
        leftFiles.insert(leftBase.relativeFilePath(fi.absoluteFilePath()), fi);
    }
    QDirIterator rightIt(rightDir, filters, flags);
    while (rightIt.hasNext()) {
        rightIt.next();
        const QFileInfo fi = rightIt.fileInfo();
        rightFiles.insert(rightBase.relativeFilePath(fi.absoluteFilePath()), fi);
    }

    QSet<QString> allPaths;
    for (auto it = leftFiles.constBegin(); it != leftFiles.constEnd(); ++it)
        allPaths.insert(it.key());
    for (auto it = rightFiles.constBegin(); it != rightFiles.constEnd(); ++it)
        allPaths.insert(it.key());

    QStringList sortedPaths(allPaths.constBegin(), allPaths.constEnd());
    std::sort(sortedPaths.begin(), sortedPaths.end());

    QVector<SyncEntry> result;
    result.reserve(sortedPaths.size());

    for (const QString &rel : sortedPaths) {
        SyncEntry entry;
        entry.relativePath = rel;
        const bool hasLeft = leftFiles.contains(rel);
        const bool hasRight = rightFiles.contains(rel);

        if (hasLeft) {
            entry.leftSize = leftFiles[rel].size();
            entry.leftModified = leftFiles[rel].lastModified();
        }
        if (hasRight) {
            entry.rightSize = rightFiles[rel].size();
            entry.rightModified = rightFiles[rel].lastModified();
        }

        if (hasLeft && !hasRight) {
            entry.status = SyncEntry::Status::LeftOnly;
        } else if (!hasLeft && hasRight) {
            entry.status = SyncEntry::Status::RightOnly;
        } else {
            const bool sizeDiffers = entry.leftSize != entry.rightSize;
            // 2s tolerance for filesystem mtime-granularity/timezone quirks
            // between differing filesystems (e.g. copying across FAT/ext4).
            const bool timeDiffers =
                qAbs(entry.leftModified.msecsTo(entry.rightModified)) > 2000;
            entry.status = (sizeDiffers || timeDiffers) ? SyncEntry::Status::Different
                                                          : SyncEntry::Status::Same;
        }
        result.append(entry);
    }
    return result;
}
