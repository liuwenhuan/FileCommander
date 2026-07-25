#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

struct SyncEntry {
    enum class Status { LeftOnly, RightOnly, Different, Same };

    QString relativePath;
    Status status = Status::Same;
    qint64 leftSize = -1;  // -1 = doesn't exist on that side
    qint64 rightSize = -1;
    QDateTime leftModified;
    QDateTime rightModified;
};

// SyncScanner emits batches of these across a thread boundary, and Qt refuses to
// queue an argument type it doesn't know -- silently dropping the signal. The
// declaration must sit with the type itself so any future cross-thread user gets
// it for free.
Q_DECLARE_METATYPE(SyncEntry)
Q_DECLARE_METATYPE(QVector<SyncEntry>)

// Compares two directory trees by relative path, size, and modification
// time (a "quick compare" -- not a byte-for-byte content diff, matching
// what the reference spec's Synchronize Directories feature does). Only
// files are reported as entries; directories are traversed into but not
// listed themselves.
class DirectorySync {
public:
    // How far apart two modification times may be while still counting as the
    // same instant.
    //
    // Measured, not assumed. Against a real SMB server (libsmbclient via
    // SmbProvider) every timestamp came back on a whole second (msec == 0) with
    // no timezone skew, and a local->remote upload round-tripped with only the
    // sub-second part truncated: |delta| < 1s. FAT/exFAT is the wider case, at a
    // documented 2-second granularity in its on-disk directory entry -- that
    // figure comes from the format specification and was NOT measured here, so
    // the tolerance keeps its historical 2s rather than being tightened to the
    // SMB number.
    static constexpr qint64 kTimeToleranceMs = 2000;

    // The single classification rule, shared by the batch compare() below and by
    // SyncScanner's streaming walk, so the two can never drift apart.
    static SyncEntry::Status classify(qint64 leftSize, const QDateTime &leftModified, bool hasLeft,
                                       qint64 rightSize, const QDateTime &rightModified,
                                       bool hasRight);

    // Blocking whole-tree compare over the local filesystem. Kept for callers
    // (and tests) that want the complete result in one call; the interactive
    // dialog drives SyncScanner directly instead so it can show progress while
    // the walk is still running.
    static QVector<SyncEntry> compare(const QString &leftDir, const QString &rightDir,
                                       bool recursive);
};
