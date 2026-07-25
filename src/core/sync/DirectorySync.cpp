#include "DirectorySync.h"

#include <QtGlobal>

#include <atomic>
#include <memory>

#include "SyncScanner.h"

SyncEntry::Status DirectorySync::classify(qint64 leftSize, const QDateTime &leftModified,
                                           bool hasLeft, qint64 rightSize,
                                           const QDateTime &rightModified, bool hasRight) {
    if (hasLeft && !hasRight)
        return SyncEntry::Status::LeftOnly;
    if (!hasLeft && hasRight)
        return SyncEntry::Status::RightOnly;

    const bool sizeDiffers = leftSize != rightSize;
    const bool timeDiffers = qAbs(leftModified.msecsTo(rightModified)) > kTimeToleranceMs;
    return (sizeDiffers || timeDiffers) ? SyncEntry::Status::Different : SyncEntry::Status::Same;
}

QVector<SyncEntry> DirectorySync::compare(const QString &leftDir, const QString &rightDir,
                                           bool recursive) {
    // Drive the streaming scanner to completion and collect everything it emits,
    // so traversal and classification live in exactly one place. The scanner is
    // used directly rather than moved onto a thread: process() is an ordinary
    // blocking call here, and the direct connection below appends synchronously
    // on this thread.
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    SyncScanner scanner(leftDir, /*leftProvider=*/nullptr, rightDir, /*rightProvider=*/nullptr,
                        recursive, cancel);

    QVector<SyncEntry> result;
    QObject::connect(&scanner, &SyncScanner::entriesReady,
                     [&result](quint64, const QVector<SyncEntry> &batch) { result.append(batch); });
    scanner.process();
    return result;
}
