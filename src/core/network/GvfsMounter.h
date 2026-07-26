#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVector>

#include "FileProvider.h" // RemoteLocation

// GvfsMounter — the bridge from a network provider's own paths to a REAL path
// on this machine.
//
// Directory browsing does NOT go through here and must not: the native backends
// (libsmbclient / libcurl / libssh2) list a directory in one round trip, while
// gvfs answers a FUSE readdir with one getattr per entry -- 139.7 ms per entry
// over SFTP on a 120 ms link. gvfs is used for the opposite case: when a real
// filename is the only thing that will do.
//
// Two situations need that, and neither has a workaround:
//   * Handing a file to an external program. VLC and text editors cannot reach
//     into this process's providers, a large share of .desktop entries take
//     only %f (a path, never a URL), and a downloaded copy is a dead end for
//     anything the user then edits and expects to save back.
//   * Libraries that open by filename -- libarchive, unsquashfs, 7z. Measured
//     against real servers: reading a 686 MB 7z off SMB took 10.64 s when
//     downloaded first and 44 ms through the mount point (240x); a 24.8 MB
//     WebDAV zip, 430 ms versus 325 ms.
//
// The mount state itself lives in the gvfs daemon, so everything here is static
// and queries it on demand; the only local state is a small cache of resolved
// mount roots (see forgetMounts()).
//
// `gio` is driven as a subprocess rather than through the GIO C API on purpose:
// it keeps the compile-time dependency at zero (only the gvfs-backends runtime
// package is needed) and it is the supported way to answer the mount daemon's
// interactive credential prompts.
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

    // A gvfs mount that would cover a given provider path, before we know
    // whether it exists yet.
    //
    // `remoteRoot` is the part that makes this more than a URI: a gvfs mount is
    // not always rooted at the server's root. An SMB mount is rooted at a
    // *share*, so the first segment of "/home/docs" is part of the mount's
    // identity rather than a directory inside it; a WebDAV mount is rooted at
    // whatever prefix it was mounted with. Subtracting remoteRoot from a
    // provider path is what turns it into a path under the mount point.
    struct MountTarget {
        QString uri;         // gvfs URI naming the mount root
        QString remoteRoot;  // provider path this mount's root corresponds to
        // Whether this candidate carries our own username, and so may be
        // *created*. Candidates without it exist only to notice a mount the
        // desktop already made (gvfs records the username in a mount's identity
        // only when the URI carried one), and are never mounted by us: gio's
        // prompt order changes when the URI has no username, and answering it
        // blind risks sending the password to the username prompt.
        bool ownCredentials = true;
    };

    // A resolved, existing mount: where its root sits locally, and which
    // provider path that root corresponds to.
    struct ResolvedMount {
        QString remoteRoot;
        QString localRoot;
        bool isValid() const { return !localRoot.isEmpty(); }
    };

    // Behaviour switches for localPathFor().
    enum ResolveFlag {
        // Mount the location if it isn't mounted yet, using the credentials the
        // provider already holds, without prompting the user again. Without it
        // an unmounted location simply yields an empty string.
        MountIfNeeded = 0x1,
        // Only return a path that actually resolves to something right now.
        // Costs one stat over the link. Drop it when the path is a file about
        // to be *created* rather than read.
        RequireExists = 0x2,
    };
    Q_DECLARE_FLAGS(ResolveFlags, ResolveFlag)

    // ================= the capability ====================================

    // The real local path for `remotePath` on `provider`'s server, or an empty
    // string when there isn't one.
    //
    // Empty is a normal answer, not an error: the provider may not be a network
    // connection at all, the location may be unmountable, gvfs may not be
    // installed, or a mount may have vanished under us (which does happen).
    // Every caller must have a working fallback -- streaming the file through
    // the provider -- and take this only as an optimisation.
    //
    // Blocking: it can spawn `gio` and, on a cold connection, mount. Call it off
    // the GUI thread, or accept a stall of roughly a round trip (measured 15 ms
    // on a LAN, 160 ms over a 120 ms SFTP link) plus mount time on the first use.
    static QString localPathFor(const FileProvider *provider, const QString &remotePath,
                                ResolveFlags flags = ResolveFlags(MountIfNeeded | RequireExists));

    // Same, from an already-extracted location. Useful when the caller holds the
    // connection details rather than the provider object.
    static QString localPathFor(const RemoteLocation &loc, const QString &remotePath,
                                ResolveFlags flags = ResolveFlags(MountIfNeeded | RequireExists));

    // Whether this connection is mounted right now, without mounting it.
    // Cheap: a directory read of the gvfs root, no network traffic.
    static bool isMounted(const RemoteLocation &loc, const QString &remotePath);

    // Drops the cached mount-root lookups. The cache only ever holds a mount
    // root's local path, and every read of it re-checks that the mount is still
    // listed, so this is for tests and for after an explicit unmount.
    static void forgetMounts();

    // ================= gvfs mechanics ====================================

    // Default TCP port for a protocol (0 = unspecified / use protocol default).
    static int defaultPort(Protocol protocol);
    // Same, keyed by the gvfs URI scheme ("sftp"/"smb"/"ftp"/"dav"/"davs").
    static int defaultPortForScheme(const QString &scheme);

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

    // The gvfs URI addressing `remotePath` on `loc`'s server, percent-encoded so
    // spaces and non-ASCII survive. Empty when `loc` is invalid.
    static QString uriForPath(const RemoteLocation &loc, const QString &remotePath);

    // Every mount that could cover `remotePath`, shallowest root first so one
    // mount serves the whole connection rather than one per directory. Pure.
    //
    // WebDAV is the awkward one: many servers refuse to be mounted at "/" (the
    // one tested here answers "not a WebDAV share"), and the connection itself
    // carries no notion of where its root is, so the candidates walk the first
    // couple of path segments and the first that mounts wins.
    static QVector<MountTarget> mountTargets(const RemoteLocation &loc,
                                             const QString &remotePath);

    // The lines to feed `gio mount <uri>` on stdin, in the order it asks for
    // them. Pure, because getting the order wrong is not a harmless mistake:
    // gio re-prompts, so a misordered answer becomes a failed login attempt at
    // the server with the *password* sent as the username.
    //
    // The order is not the same for every protocol, and it changes with the URI:
    //   SMB,  no user in the URI:  User, Domain, Password
    //   SMB,  user in the URI:     Domain, Password
    //   others, no user in the URI: User, Password
    //   others, user in the URI:    Password
    // Anonymous connections answer nothing; mount() passes `gio mount -a`.
    static QStringList mountAnswers(const RemoteLocation &loc, const QString &uri);

    // Mount `uri`, answering gio's prompts from `loc`'s credentials so the user
    // is not asked for a password the app already has.
    static MountResult mount(const QString &uri, const RemoteLocation &loc,
                             int timeoutMs = 45000);

    // Unmount a previously mounted URI (`gio mount -u <uri>`). Returns true on
    // success.
    static bool unmount(const QString &uri, int timeoutMs = 15000);

    // Resolve the local mount path for an already-mounted URI. Asks `gio info`
    // first (authoritative -- it also proves the mount is alive), then falls
    // back to matching the URI against the directories actually present under
    // the gvfs root. Returns empty if not mounted.
    static QString localPathForUri(const QString &uri);

    // Parse `gio mount -li` into a list of active mounts.
    static QVector<MountInfo> listMounts();

    // List browsable network-neighbourhood locations (`gio list network:///`).
    static QVector<NetworkLocation> networkLocations();

    // --- helpers exposed for unit testing --------------------------------

    // The gvfs mount-directory name for a URI, as gvfs actually spells it:
    //   sftp:host=example.com,user=bob
    //   smb-share:server=nas,share=media          (server and share lowercased)
    //   dav:host=example.com,port=5006,ssl=false,prefix=%2Fdav
    //   ftp:host=example.com
    // Returns empty for an unparseable URI. This is a *convention*, so nothing
    // depends on it alone -- see matchMountDir(), which compares it field by
    // field against the directories that really exist.
    static QString gvfsMountDirName(const QString &uri);

    // The entry from `dirNames` (the gvfs root's directory listing) that names
    // the same mount as `uri`, or empty if none does. Compares the mount spec
    // field by field rather than as a string, so key order does not matter and
    // gvfs's own case-folding of server and share names is tolerated.
    //
    // The key set must match exactly: a mount recorded with `user=bob` is a
    // different, separately authenticated session from one without, and quietly
    // borrowing the wrong one would read a share with somebody else's rights.
    static QString matchMountDir(const QStringList &dirNames, const QString &uri);

    // Splits `path` into the part under `root`, POSIX-style. Returns false when
    // `path` is not under `root` at a segment boundary ("/ab" is not under
    // "/a"); *rel is then untouched. An exact match yields an empty *rel.
    static bool relativeUnder(const QString &root, const QString &path, QString *rel);

    // Root of the current user's gvfs mounts (/run/user/<uid>/gvfs).
    static QString gvfsRoot();

    // The environment `gio` is run with: gio's output is parsed, so its messages
    // are forced to English, but the character encoding is left alone because
    // the paths it prints contain real filenames.
    //
    // This is the whole of a bug that made localPathForUri() useless on any
    // non-English desktop: it looked for "local path:" while a zh_CN session
    // prints "本地路径:". Setting LC_ALL=C fixes the label but then mangles every
    // non-ASCII filename in the answer to "?", so the message locale is set on
    // its own and LC_ALL -- which would override it -- is removed.
    static QProcessEnvironment gioEnvironment(const QProcessEnvironment &base);

    // Parse the textual output of `gio mount -li` (injected for tests).
    static QVector<MountInfo> parseMountList(const QString &output);

    // Parse the textual output of `gio list network:///` (injected for tests).
    static QVector<NetworkLocation> parseNetworkList(const QString &output);

    // Pull the local path out of `gio info` output (injected for tests).
    static QString parseLocalPath(const QString &output);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(GvfsMounter::ResolveFlags)
