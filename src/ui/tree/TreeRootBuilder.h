#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// RemovableDevice is used by value inside a QVector parameter below, so the
// full definition is needed rather than a forward declaration.
#include "devices/RemovableDeviceMonitor.h"

// One top-level entry in the folder tree. Roots stand for a place a file tree
// can start from: the local filesystem, a fixed disk, an attached removable
// volume, or a live server connection.
struct TreeRoot {
    enum Kind {
        LocalFilesystem, // the whole local tree, rooted at "/" (single-disk case)
        LocalVolume,     // one fixed disk, rooted at its mount point
        Removable,       // an attached USB stick / card / phone / external drive
        Network,         // a live server connection, browsed through its provider
    };

    Kind kind = LocalFilesystem;
    QString label;        // row text: volume name, device label, or "user@host"
    QString iconName;     // ":/icons/<iconName>.svg"; empty => generic folder
    QString basePath;     // where this root's tree starts (local path, or the
                          // provider-relative path for a network root)
    QString connectionId; // network roots only: which connection this belongs to
    QString deviceId;     // removable roots only: UDisks object path, for diffing
    // Network roots only: successively deeper starting points, from the topmost
    // candidate down to the directory the tab is actually in. The tree tries
    // basePath first and falls back to the next candidate when the server denies
    // the listing, so the root ends up at the highest directory the user can
    // actually see rather than failing outright or starting needlessly deep.
    QStringList basePathFallbacks;
    // Network roots owned by the OTHER panel are shown (so the user can see what
    // is connected) but cannot be activated: adopting another panel's session
    // would break per-tab connection ownership, and silently dialling a second
    // connection would make a click cost a full (possibly slow) connect.
    bool activatable = true;

    bool operator==(const TreeRoot &other) const {
        return kind == other.kind && label == other.label && iconName == other.iconName
               && basePath == other.basePath && connectionId == other.connectionId
               && deviceId == other.deviceId && activatable == other.activatable;
    }
};

// A mounted local filesystem that is not removable -- an internal disk.
struct LocalVolume {
    QString name;       // volume label, or the device node when unlabelled
    QString mountPoint; // "/", "/home", "/mnt/data", ...
};

// A live server connection, as seen by the tree.
struct NetworkTreeEntry {
    QString connectionId; // "smb://user@host"; stable identity of the connection
    QString label;        // "user@host", snapshotted at registration time
    QString scheme;       // "smb" / "sftp" / "ftp" / "webdav"
    QString basePath;     // topmost visible directory, provider-relative
    // Deeper starting points to fall back to when basePath is not listable; see
    // TreeRoot::basePathFallbacks.
    QStringList basePathFallbacks;
    bool ownedByThisPanel = true; // false => shown greyed out (see activatable)
};

// Assembles the tree's top-level rows from the three device/connection sources.
// Pure logic with no Qt GUI or I/O dependency, so the assembly rules -- above
// all the single-disk degradation below -- are unit-testable directly.
class TreeRootBuilder {
public:
    // The ordering is stable and deliberate: local disks, then removable
    // volumes, then network connections.
    //
    // Degradation rule: exactly one local volume, nothing removable attached and
    // nothing connected yields a single LocalFilesystem root rooted at "/". That
    // is the overwhelmingly common case, and it must keep behaving exactly like
    // the plain local file tree did -- no device header row appears just because
    // the tree learned about devices.
    static QVector<TreeRoot> build(const QVector<LocalVolume> &volumes,
                                   const QVector<RemovableDevice> &devices,
                                   const QVector<NetworkTreeEntry> &networks);

    // Enumerates mounted non-removable local filesystems via QStorageInfo,
    // excluding pseudo-filesystems and anything mounted from a removable device
    // (those arrive through RemovableDeviceMonitor instead, with a better label
    // and an eject affordance). `removableMounts` are the mount points to skip.
    static QVector<LocalVolume> enumerateLocalVolumes(const QStringList &removableMounts);

    // The candidate starting points for a network root, topmost first: "/" then
    // each successively deeper prefix of `currentPath`. The tree walks this list
    // until one lists successfully, which is how "start from the highest visible
    // directory" is realised without a blocking probe.
    //
    // Note the limit of what this can mean: a connection may itself be anchored
    // below the server's real root (a WebDAV endpoint such as
    // /remote.php/dav/files/user is "/" as far as the provider is concerned).
    // The topmost directory reachable here is that anchor, not the server root;
    // no protocol call can go above it.
    static QStringList networkBaseCandidates(const QString &currentPath);
};
