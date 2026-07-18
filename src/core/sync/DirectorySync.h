#pragma once

#include <QDateTime>
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

// Compares two directory trees by relative path, size, and modification
// time (a "quick compare" -- not a byte-for-byte content diff, matching
// what the reference spec's Synchronize Directories feature does). Only
// files are reported as entries; directories are traversed into but not
// listed themselves.
class DirectorySync {
public:
    static QVector<SyncEntry> compare(const QString &leftDir, const QString &rightDir,
                                       bool recursive);
};
