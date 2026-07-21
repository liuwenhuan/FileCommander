#pragma once

#include <QString>
#include <QVector>

// GvfsMounter — phase 1 network integration skeleton.
//
// Mounts remote filesystems (SFTP / SMB / WebDAV / FTP) through GVfs by shelling
// out to the `gio` command-line tool (part of gvfs / glib). GVfs exposes each
// mount as a local path under /run/user/<uid>/gvfs/, which the existing
// FileSystemModel can browse like any other directory — no special handling in
// the file panels is required.
//
// Design notes for later phases:
//   * We deliberately drive the `gio` CLI via QProcess rather than link the GIO
//     C API. This keeps the compile-time dependency surface at zero (only the
//     runtime package gvfs-backends is needed) and is the most robust way to get
//     interactive password prompts handled by the gvfs mount daemon.
//   * Password handling is a TODO: phase 1 supports anonymous and interactive
//     mounts only. A future phase should store/retrieve credentials via
//     libsecret and feed them to `gio mount` on stdin (see mount()).
//   * All methods are static and stateless — the authoritative mount state lives
//     in the gvfs daemon, which we query on demand.
class GvfsMounter {
public:
    enum class Protocol {
        Sftp,
        Smb,
        WebDav,   // http-backed  -> dav://
        WebDavs,  // https-backed -> davs://
        Ftp,
    };

    // Result of a mount attempt.
    struct MountResult {
        bool success = false;
        QString uri;        // the URI we asked gio to mount
        QString localPath;  // /run/user/<uid>/gvfs/... (empty on failure)
        QString error;      // human-readable error (empty on success)
    };

    // One entry parsed out of `gio mount -l` / `gio mount -li`.
    struct MountInfo {
        QString name;       // display name reported by gio
        QString uri;        // activation URI, e.g. sftp://user@host/
        QString localPath;  // local mount path if known, else empty
    };

    // A browsable network neighbourhood location (from `gio list network:///`).
    struct NetworkLocation {
        QString displayName;
        QString uri;  // e.g. smb://server/ or network://...
    };

    // Default TCP port for a protocol (0 = unspecified / use protocol default).
    static int defaultPort(Protocol protocol);

    // Scheme string for a protocol ("sftp", "smb", "dav", "davs", "ftp").
    static QString scheme(Protocol protocol);

    // Build a GVfs URI from connection parameters.
    //   SFTP  -> sftp://user@host:port/path
    //   SMB   -> smb://host/share          (path is treated as the share/path)
    //   WebDAV-> dav://host:port/path  (http) or davs://host:port/path (https)
    //   FTP   -> ftp://host:port/path
    // When `user` is empty (anonymous) the userinfo is omitted. When `port` is
    // <= 0 or equals the protocol default, the port is omitted from the URI.
    // `path` is normalised to start with a single leading slash.
    static QString buildUri(Protocol protocol, const QString &host, int port,
                            const QString &user, const QString &path);

    // Mount `uri` via `gio mount <uri>`. If `password` is non-empty it is fed to
    // the process stdin to satisfy an interactive prompt (best-effort; anonymous
    // and passwordless key-based auth are the primary supported paths in phase 1).
    // TODO(phase-2): source credentials from libsecret instead of a parameter.
    static MountResult mount(const QString &uri, const QString &password = QString(),
                             int timeoutMs = 30000);

    // Unmount a previously mounted URI (`gio mount -u <uri>`). Returns true on
    // success.
    static bool unmount(const QString &uri, int timeoutMs = 15000);

    // Resolve the local mount path for an already-mounted URI. Tries `gio info`
    // first (authoritative), then falls back to constructing the conventional
    // gvfs directory name and checking it exists. Returns empty if not mounted.
    static QString localPathForUri(const QString &uri);

    // Parse `gio mount -li` into a list of active mounts.
    static QVector<MountInfo> listMounts();

    // List browsable network-neighbourhood locations (`gio list network:///`).
    // Phase 1: enumeration only.
    static QVector<NetworkLocation> networkLocations();

    // --- helpers exposed for unit testing --------------------------------

    // The gvfs directory name convention, e.g.
    //   sftp:host=example.com,user=bob
    //   smb-share:server=nas,share=media
    //   dav:host=example.com,ssl=true
    //   ftp:host=example.com
    // Returns empty for an unparseable URI.
    static QString gvfsMountDirName(const QString &uri);

    // Root of the current user's gvfs mounts (/run/user/<uid>/gvfs).
    static QString gvfsRoot();

    // Parse the textual output of `gio mount -li` (injected for tests).
    static QVector<MountInfo> parseMountList(const QString &output);

    // Parse the textual output of `gio list network:///` (injected for tests).
    static QVector<NetworkLocation> parseNetworkList(const QString &output);
};
