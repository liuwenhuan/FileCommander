#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

// One row of the "Computer" listing: a place the user can go that is not a
// directory inside another directory. Drives, the standard user folders, saved
// server bookmarks, plugged-in removable media and discovered network hosts all
// reduce to this shape.
//
// `kind` is what the panel dispatches on when the row is activated -- a drive is
// a plain local navigation while a saved bookmark has to open a connection --
// which is why it is carried explicitly instead of being re-derived from the
// target string. Guessing from the string is exactly the "path == local
// filesystem path" trap: a server named like a directory would be indis-
// tinguishable.
struct ComputerEntry {
    enum class Kind {
        Drive,           // a mounted fixed volume; target is its root path
        UserFolder,      // Desktop/Documents/..., target is the local path
        RemovableDevice, // target is the RemovableDeviceMonitor id, NOT a path
        SavedServer,     // target is the SavedConnection id
        NetworkHost,     // target is the host name or address to browse
    };

    Kind kind = Kind::Drive;
    QString name;      // what the Name column shows
    QString target;    // meaning depends on kind; see above
    QString iconPath;  // ":/icons/....svg"; empty = let IconCache decide
    QDateTime created; // only saved bookmarks have one; invalid renders blank
    // Set for drives so the row can say how full the volume is. -1 when unknown
    // (an unreadable or not-ready volume), which renders as nothing rather than
    // as "0 bytes free".
    qint64 bytesTotal = -1;
    qint64 bytesFree = -1;
};

// So a ComputerEntry can cross a queued signal/slot connection and sit in a
// QVariant, like FileInfo above it.
Q_DECLARE_METATYPE(ComputerEntry)

// Builds the fixed part of the computer listing -- everything that can be
// enumerated from this process without a device monitor or a network scan.
// Removable media and discovered hosts are appended by the UI layer, which owns
// those monitors.
namespace ComputerCatalog {


// Mounted fixed volumes. On Windows these are the drive letters with their
// volume labels; on Linux they are the real block-device filesystems, named
// "<label> (<mount point>)" -- by mount point alone when the filesystem carries
// no label. Left out: pseudo filesystems (tmpfs, proc, cgroup, snap loopbacks,
// ...), which are not drives in any sense the user means; the operating
// system's own mount points (/boot, /var, /sysroot, an ostree layout's
// /persistent, ...); and every mount of a device beyond the one best mount
// point for it, so a disk bind-mounted to five places is still one row.
//
// `includeSystemVolumes` turns both of those last two off -- every mount of
// every real filesystem, listed separately. It exists for the "Show System
// Partitions" setting; the UI layer passes the user's choice, so this layer
// never reads Settings itself.
QVector<ComputerEntry> drives(bool includeSystemVolumes = false);

// Desktop / Documents / Pictures / Music / Videos / Downloads, in that order,
// skipping any the platform does not define or that does not exist.
QVector<ComputerEntry> userFolders();

// Saved server bookmarks, newest-saved order preserved from the store.
QVector<ComputerEntry> savedServers();

// drives() + userFolders() + savedServers(), i.e. everything this layer knows.
QVector<ComputerEntry> localAndSaved(bool includeSystemVolumes = false);

} // namespace ComputerCatalog
