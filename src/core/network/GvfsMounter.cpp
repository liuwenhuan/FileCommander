#include "GvfsMounter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>

#include <dirent.h>
#include <unistd.h> // getuid()

namespace {

// Run `gio <args>`, feeding `stdinLines` (one per line) if provided. Returns the
// process exit code (or -1 if it failed to start / timed out); fills
// stdout/stderr.
int runGio(const QStringList &args, QString *stdOut, QString *stdErr, int timeoutMs,
           const QStringList &stdinLines = QStringList()) {
    QProcess proc;
    proc.setProcessEnvironment(
        GvfsMounter::gioEnvironment(QProcessEnvironment::systemEnvironment()));
    proc.start(QStringLiteral("gio"), args);
    if (!proc.waitForStarted(timeoutMs)) {
        if (stdErr)
            *stdErr = QStringLiteral("failed to start `gio` — is gvfs installed?");
        return -1;
    }
    for (const QString &line : stdinLines) {
        proc.write(line.toUtf8());
        proc.write("\n");
    }
    proc.closeWriteChannel();
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        if (stdErr)
            *stdErr = QStringLiteral("`gio` timed out");
        return -1;
    }
    if (stdOut)
        *stdOut = QString::fromUtf8(proc.readAllStandardOutput());
    if (stdErr) {
        const QString err = QString::fromUtf8(proc.readAllStandardError());
        if (!err.isEmpty())
            *stdErr = err.trimmed();
    }
    return proc.exitCode();
}

// Normalise a remote path to a single leading slash; empty -> "/".
QString normalisePath(const QString &path) {
    QString p = path.trimmed();
    while (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    return QLatin1Char('/') + p;
}

// Collapse "//" and drop a trailing slash, so "/a//b/" and "/a/b" compare equal.
// "/" stays "/".
QString canonicalRemotePath(const QString &path) {
    const QStringList segs =
        path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segs.isEmpty())
        return QStringLiteral("/");
    return QLatin1Char('/') + segs.join(QLatin1Char('/'));
}

// A gvfs mount spec: the "type" that prefixes a mount directory name, plus its
// key=value fields in the order gvfs itself writes them.
struct MountSpec {
    QString type; // "sftp" | "ftp" | "smb-share" | "dav"
    QVector<QPair<QString, QString>> fields;
    bool isValid() const { return !type.isEmpty(); }
};

// The mount spec gvfs uses for `uri`. Empty type when the URI cannot name a
// mount (no host, or an SMB URI with no share -- the share list is a different
// backend with no file to hand out).
MountSpec specForUri(const QString &uri) {
    MountSpec spec;
    const QUrl url(uri);
    if (!url.isValid() || url.host().isEmpty())
        return spec;

    const QString sch = url.scheme().toLower();
    const QString host = url.host().toLower();
    const QString user = url.userName();
    const int port = url.port();
    const QStringList segs = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);

    if (sch == QLatin1String("smb")) {
        // An SMB mount is per share, so the first path segment is part of the
        // mount's identity. gvfs lower-cases both the server and the share:
        // mounting smb://192.0.2.10/Download really produces
        // "smb-share:server=192.0.2.10,share=download".
        if (segs.isEmpty())
            return spec;
        spec.type = QStringLiteral("smb-share");
        spec.fields.append({QStringLiteral("server"), host});
        spec.fields.append({QStringLiteral("share"), segs.first().toLower()});
        if (!user.isEmpty())
            spec.fields.append({QStringLiteral("user"), user});
        return spec;
    }

    if (sch == QLatin1String("dav") || sch == QLatin1String("davs")) {
        // Both transports share one mount type and are told apart by ssl=.
        // The path becomes `prefix=`, percent-encoded (so "/dav" -> "%2Fdav"),
        // and is absent entirely for a mount rooted at "/".
        spec.type = QStringLiteral("dav");
        spec.fields.append({QStringLiteral("host"), host});
        if (port > 0)
            spec.fields.append({QStringLiteral("port"), QString::number(port)});
        spec.fields.append({QStringLiteral("ssl"), sch == QLatin1String("davs")
                                                       ? QStringLiteral("true")
                                                       : QStringLiteral("false")});
        if (!user.isEmpty())
            spec.fields.append({QStringLiteral("user"), user});
        if (!segs.isEmpty()) {
            const QString prefix = QLatin1Char('/') + segs.join(QLatin1Char('/'));
            spec.fields.append({QStringLiteral("prefix"),
                                QString::fromLatin1(QUrl::toPercentEncoding(prefix))});
        }
        return spec;
    }

    if (sch != QLatin1String("sftp") && sch != QLatin1String("ftp"))
        return spec; // not a scheme we know how to name

    // sftp / ftp: the mount is the whole server, the path is inside it.
    spec.type = sch;
    spec.fields.append({QStringLiteral("host"), host});
    if (port > 0)
        spec.fields.append({QStringLiteral("port"), QString::number(port)});
    if (!user.isEmpty())
        spec.fields.append({QStringLiteral("user"), user});
    return spec;
}

// Parse a gvfs mount directory name ("smb-share:server=nas,share=media") back
// into a spec. Invalid (empty type) when it isn't of that shape.
MountSpec specForDirName(const QString &dirName) {
    MountSpec spec;
    const int colon = dirName.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return spec;
    const QString type = dirName.left(colon);
    const QStringList pairs =
        dirName.mid(colon + 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (pairs.isEmpty())
        return spec;
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return MountSpec(); // not a key=value list -- not a mount dir
        spec.fields.append({pair.left(eq), pair.mid(eq + 1)});
    }
    spec.type = type;
    return spec;
}

// Whether two specs name the same mount. The key SET must match exactly: a
// mount recorded with user=bob is a separately authenticated session from one
// without, and silently borrowing the wrong one would read the share with
// somebody else's rights. Values compare case-insensitively because gvfs
// case-folds server and share names and writes percent escapes in either case.
bool specsMatch(const MountSpec &a, const MountSpec &b) {
    if (!a.isValid() || !b.isValid())
        return false;
    if (a.type.compare(b.type, Qt::CaseInsensitive) != 0)
        return false;
    if (a.fields.size() != b.fields.size())
        return false;
    for (const auto &fa : a.fields) {
        bool found = false;
        for (const auto &fb : b.fields) {
            if (fa.first.compare(fb.first, Qt::CaseInsensitive) != 0)
                continue;
            if (fa.second.compare(fb.second, Qt::CaseInsensitive) != 0)
                return false;
            found = true;
            break;
        }
        if (!found)
            return false;
    }
    return true;
}

// The directory names directly under the gvfs root, read with readdir and
// nothing else. Deliberately not QDir::entryList: applying any filter makes Qt
// stat every entry, and a stat on a mount root is a network round trip (107 ms
// on the SFTP link tested here) that can also hang on a dead mount. readdir
// alone is served locally by gvfsd-fuse in about 2 ms.
QStringList gvfsRootEntries() {
    QStringList names;
    const QByteArray root = GvfsMounter::gvfsRoot().toLocal8Bit();
    DIR *dir = ::opendir(root.constData());
    if (!dir)
        return names;
    while (struct dirent *entry = ::readdir(dir)) {
        const QString name = QString::fromLocal8Bit(entry->d_name);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        names.append(name);
    }
    ::closedir(dir);
    return names;
}

// --- resolved-mount cache -----------------------------------------------
//
// Only ever holds where a mount root sits locally. Every read re-checks that
// the directory is still listed under the gvfs root, which is the cheap,
// network-free way to notice a mount that has gone away underneath us.
struct CachedMount {
    QString remoteRoot;
    QString localRoot;
};

QMutex g_cacheMutex;
QHash<QString, CachedMount> g_cache;

// Identity of the *connection*, which for SMB includes the share (one gvfs
// mount per share) and for everything else does not.
QString connectionKey(const RemoteLocation &loc, const QString &remotePath) {
    QString key = loc.scheme.toLower() + QLatin1Char('|') + loc.host.toLower() +
                  QLatin1Char('|') + QString::number(loc.port) + QLatin1Char('|') +
                  loc.user;
    if (loc.scheme.compare(QLatin1String("smb"), Qt::CaseInsensitive) == 0) {
        const QStringList segs =
            remotePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        key += QLatin1Char('|') + (segs.isEmpty() ? QString() : segs.first().toLower());
    }
    return key;
}

// How long a freshly created mount is given to become usable through FUSE.
// Measured worst case on this machine: 1.1 s, when the same process had used an
// earlier incarnation of the same mount.
constexpr int kMountVisibleBudgetMs = 4000;

// Block until `localRoot` can actually be stat'ed, or the budget runs out.
bool waitForMountPoint(const QString &localRoot, int budgetMs) {
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        if (QFileInfo::exists(localRoot))
            return true;
        if (timer.elapsed() >= budgetMs)
            return false;
        QThread::msleep(50);
    }
}

bool mountStillListed(const QString &localRoot) {
    if (localRoot.isEmpty())
        return false;
    return gvfsRootEntries().contains(QFileInfo(localRoot).fileName());
}

// Find (and, if allowed, create) the gvfs mount whose root covers `remotePath`.
GvfsMounter::ResolvedMount resolveMount(const RemoteLocation &loc, const QString &remotePath,
                                        bool mountIfNeeded) {
    GvfsMounter::ResolvedMount resolved;
    if (!loc.isValid())
        return resolved;

    const QString key = connectionKey(loc, remotePath);
    CachedMount cached;
    {
        QMutexLocker locker(&g_cacheMutex);
        cached = g_cache.value(key);
    }
    // Re-check outside the lock: the directory read is a syscall, and a wedged
    // gvfsd-fuse must not be able to block every other thread's lookup.
    if (!cached.localRoot.isEmpty()) {
        if (mountStillListed(cached.localRoot)) {
            resolved.remoteRoot = cached.remoteRoot;
            resolved.localRoot = cached.localRoot;
            return resolved;
        }
        QMutexLocker locker(&g_cacheMutex);
        g_cache.remove(key); // vanished under us; fall through and re-resolve
    }

    const QVector<GvfsMounter::MountTarget> targets =
        GvfsMounter::mountTargets(loc, remotePath);
    if (targets.isEmpty())
        return resolved;

    const auto remember = [&key, &resolved](const QString &remoteRoot, const QString &localRoot) {
        resolved.remoteRoot = remoteRoot;
        resolved.localRoot = localRoot;
        QMutexLocker locker(&g_cacheMutex);
        g_cache.insert(key, CachedMount{remoteRoot, localRoot});
    };

    // 1. Reuse a mount that is already there -- including one the desktop made,
    //    which is why the no-username candidates are checked too. One readdir,
    //    no subprocess, no network.
    const QStringList entries = gvfsRootEntries();
    for (const auto &target : targets) {
        const QString dir = GvfsMounter::matchMountDir(entries, target.uri);
        if (!dir.isEmpty()) {
            remember(target.remoteRoot, GvfsMounter::gvfsRoot() + QLatin1Char('/') + dir);
            return resolved;
        }
    }

    // 2. Ask gio directly for the candidates we would mount. This costs a
    //    subprocess each but is authoritative: it catches a mount whose
    //    directory name does not match the convention above, and it proves the
    //    mount is actually alive rather than merely listed.
    for (const auto &target : targets) {
        if (!target.ownCredentials)
            continue;
        const QString local = GvfsMounter::localPathForUri(target.uri);
        if (!local.isEmpty()) {
            remember(target.remoteRoot, local);
            return resolved;
        }
    }

    if (!mountIfNeeded)
        return resolved;

    // 3. Mount it ourselves, shallowest root first so one mount covers the whole
    //    connection instead of one per directory.
    for (const auto &target : targets) {
        if (!target.ownCredentials)
            continue;
        const GvfsMounter::MountResult result = GvfsMounter::mount(target.uri, loc);
        if (!result.success || result.localPath.isEmpty())
            continue;
        remember(target.remoteRoot, result.localPath);
        return resolved;
    }
    return resolved;
}

} // namespace

int GvfsMounter::defaultPort(Protocol protocol) {
    switch (protocol) {
    case Protocol::Sftp:
        return 22;
    case Protocol::Smb:
        return 445;
    case Protocol::WebDav:
        return 80;
    case Protocol::WebDavs:
        return 443;
    case Protocol::Ftp:
        return 21;
    }
    return 0;
}

int GvfsMounter::defaultPortForScheme(const QString &scheme) {
    const QString s = scheme.toLower();
    if (s == QLatin1String("sftp"))
        return 22;
    if (s == QLatin1String("smb"))
        return 445;
    if (s == QLatin1String("dav"))
        return 80;
    if (s == QLatin1String("davs"))
        return 443;
    if (s == QLatin1String("ftp"))
        return 21;
    return 0;
}

QString GvfsMounter::scheme(Protocol protocol) {
    switch (protocol) {
    case Protocol::Sftp:
        return QStringLiteral("sftp");
    case Protocol::Smb:
        return QStringLiteral("smb");
    case Protocol::WebDav:
        return QStringLiteral("dav");
    case Protocol::WebDavs:
        return QStringLiteral("davs");
    case Protocol::Ftp:
        return QStringLiteral("ftp");
    }
    return QString();
}

QString GvfsMounter::buildUri(Protocol protocol, const QString &host, int port,
                              const QString &user, const QString &path) {
    const QString hostTrimmed = host.trimmed();
    if (hostTrimmed.isEmpty())
        return QString();

    const QString sch = scheme(protocol);
    QString uri = sch + QStringLiteral("://");

    // SMB uses a share-based layout (smb://host/share/...) and never carries a
    // port in the URI; userinfo is optional.
    if (protocol == Protocol::Smb) {
        if (!user.trimmed().isEmpty())
            uri += user.trimmed() + QLatin1Char('@');
        uri += hostTrimmed;
        uri += normalisePath(path);
        return uri;
    }

    if (!user.trimmed().isEmpty())
        uri += user.trimmed() + QLatin1Char('@');
    uri += hostTrimmed;

    // Only append a port when it is meaningful and non-default.
    if (port > 0 && port != defaultPort(protocol))
        uri += QLatin1Char(':') + QString::number(port);

    uri += normalisePath(path);
    return uri;
}

QString GvfsMounter::uriForPath(const RemoteLocation &loc, const QString &remotePath) {
    if (!loc.isValid())
        return QString();
    QUrl url;
    url.setScheme(loc.scheme.toLower());
    url.setHost(loc.host);
    if (!loc.user.isEmpty())
        url.setUserName(loc.user);
    const int fallback = defaultPortForScheme(loc.scheme);
    if (loc.port > 0 && loc.port != fallback)
        url.setPort(loc.port);
    // DecodedMode: the path is a real filename, so a literal '%' in it is a
    // percent sign and not the start of an escape.
    url.setPath(normalisePath(remotePath), QUrl::DecodedMode);
    if (!url.isValid())
        return QString();
    return url.toString(QUrl::FullyEncoded);
}

QVector<GvfsMounter::MountTarget> GvfsMounter::mountTargets(const RemoteLocation &loc,
                                                            const QString &remotePath) {
    QVector<MountTarget> targets;
    if (!loc.isValid())
        return targets;

    const QString path = canonicalRemotePath(remotePath);
    const QStringList segs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QString sch = loc.scheme.toLower();

    // Every root gets two candidates: ours (with the username, so the prompt
    // order is known and it is safe to answer blind) and the same root without
    // it, which is how the desktop's own mounts are recorded and so the only way
    // to notice and reuse one.
    QStringList roots;
    if (sch == QLatin1String("smb")) {
        // The share is the mount. Without one there is nothing to mount: the
        // server root is only a list of shares.
        if (segs.isEmpty())
            return targets;
        roots << QLatin1Char('/') + segs.first();
    } else if (sch == QLatin1String("dav") || sch == QLatin1String("davs")) {
        // The connection does not know where its own root is, and plenty of
        // servers refuse "/" (the one tested here answers "not a WebDAV
        // share"), so walk down from the shallowest root and take the first
        // that mounts. Shallowest first matters: it makes one mount serve the
        // whole connection instead of accumulating one per directory.
        roots << QStringLiteral("/");
        if (segs.size() >= 1)
            roots << QLatin1Char('/') + segs.first();
        if (segs.size() >= 2)
            roots << QLatin1Char('/') + segs.mid(0, 2).join(QLatin1Char('/'));
        // Last resorts for a deeply nested server root: the directory holding
        // the target, then the target itself (it may be a directory).
        if (segs.size() > 2)
            roots << QLatin1Char('/') + segs.mid(0, segs.size() - 1).join(QLatin1Char('/'));
        if (segs.size() > 2)
            roots << path;
    } else if (sch == QLatin1String("sftp") || sch == QLatin1String("ftp")) {
        roots << QStringLiteral("/");
    } else {
        return targets;
    }

    roots.removeDuplicates();
    for (const QString &root : roots) {
        MountTarget mine;
        mine.uri = uriForPath(loc, root);
        mine.remoteRoot = root;
        mine.ownCredentials = true;
        if (!mine.uri.isEmpty())
            targets.append(mine);
        if (loc.user.isEmpty())
            continue;
        RemoteLocation withoutUser = loc;
        withoutUser.user.clear();
        MountTarget theirs;
        theirs.uri = uriForPath(withoutUser, root);
        theirs.remoteRoot = root;
        theirs.ownCredentials = false;
        if (!theirs.uri.isEmpty())
            targets.append(theirs);
    }
    return targets;
}

QStringList GvfsMounter::mountAnswers(const RemoteLocation &loc, const QString &uri) {
    QStringList answers;
    if (loc.anonymous)
        return answers; // mount() passes `gio mount -a` instead

    const QUrl url(uri);
    if (url.userName().isEmpty())
        answers << loc.user; // gio asks for the username first when the URI lacks one
    if (loc.scheme.compare(QLatin1String("smb"), Qt::CaseInsensitive) == 0)
        answers << QString(); // Domain: empty accepts gio's offered default
    answers << loc.password;
    return answers;
}

QProcessEnvironment GvfsMounter::gioEnvironment(const QProcessEnvironment &base) {
    QProcessEnvironment env = base;
    // Force gio's *messages* to English, because they are parsed. LC_ALL would
    // do it in one line but also forces the character encoding to ASCII, and
    // then every non-ASCII character in the paths gio prints back comes out as
    // "?" -- so it is removed rather than set.
    env.remove(QStringLiteral("LC_ALL"));
    env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C")); // wins over LC_MESSAGES
    env.insert(QStringLiteral("LC_MESSAGES"), QStringLiteral("C"));
    // Leave the character encoding as the session has it, unless the session has
    // none, in which case ask for UTF-8 rather than let filenames be mangled.
    const QString ctype = env.value(QStringLiteral("LC_CTYPE"),
                                    env.value(QStringLiteral("LANG")));
    if (ctype.isEmpty() || ctype == QLatin1String("C") ||
        ctype == QLatin1String("POSIX")) {
        env.insert(QStringLiteral("LC_CTYPE"), QStringLiteral("C.UTF-8"));
    }
    return env;
}

QString GvfsMounter::gvfsRoot() {
    return QStringLiteral("/run/user/%1/gvfs").arg(getuid());
}

QString GvfsMounter::gvfsMountDirName(const QString &uri) {
    const MountSpec spec = specForUri(uri);
    if (!spec.isValid())
        return QString();
    QStringList parts;
    parts.reserve(spec.fields.size());
    for (const auto &field : spec.fields)
        parts << field.first + QLatin1Char('=') + field.second;
    return spec.type + QLatin1Char(':') + parts.join(QLatin1Char(','));
}

QString GvfsMounter::matchMountDir(const QStringList &dirNames, const QString &uri) {
    const MountSpec wanted = specForUri(uri);
    if (!wanted.isValid())
        return QString();
    for (const QString &name : dirNames) {
        if (specsMatch(wanted, specForDirName(name)))
            return name;
    }
    return QString();
}

bool GvfsMounter::relativeUnder(const QString &root, const QString &path, QString *rel) {
    const QString r = canonicalRemotePath(root);
    const QString p = canonicalRemotePath(path);
    if (r == QLatin1String("/")) {
        if (rel)
            *rel = p.mid(1);
        return true;
    }
    if (p == r) {
        if (rel)
            rel->clear();
        return true;
    }
    // The trailing slash is what keeps "/ab" from counting as being under "/a".
    if (!p.startsWith(r + QLatin1Char('/')))
        return false;
    if (rel)
        *rel = p.mid(r.size() + 1);
    return true;
}

GvfsMounter::MountResult GvfsMounter::mount(const QString &uri, const RemoteLocation &loc,
                                            int timeoutMs) {
    MountResult result;
    result.uri = uri;

    if (uri.trimmed().isEmpty()) {
        result.error = QStringLiteral("empty URI");
        return result;
    }

    QStringList args{QStringLiteral("mount")};
    if (loc.anonymous)
        args << QStringLiteral("-a");
    args << uri;

    QString out, err;
    const int code = runGio(args, &out, &err, timeoutMs, mountAnswers(loc, uri));

    // Ask where it landed regardless of the exit code, because a non-zero exit
    // does not mean there is no mount. "Location is already mounted" is reported
    // as a failure and is a success for us, and it turns up for a second reason
    // besides the obvious one: unmounting is asynchronous, so a remount issued
    // right after an unmount can be refused while the old mount is still going
    // away -- and then stays.
    //
    // Matching that message as text is not an option. gioEnvironment() pins the
    // locale of the strings the `gio` tool itself prints, but this one is a
    // GError raised inside gvfsd and forwarded over D-Bus, so it arrives in the
    // *desktop session's* language ("位置已挂载" here) no matter what environment
    // the client was started with. Asking where the mount is answers the only
    // question that matters and does it in any language.
    result.localPath = localPathForUri(uri);
    if (!result.localPath.isEmpty()) {
        // `gio mount` returning is not the same as the mount being usable.
        // gvfsd-fuse publishes the new directory asynchronously, and re-creating
        // a mount that this process had used before is worse than a cold one:
        // measured here, the mount point stayed un-stat-able for 1.1 s after gio
        // had already reported success (and briefly vanished from the gvfs root
        // listing in the middle, as the old entry was torn down and the new one
        // attached). Handing that path back would produce a file that cannot be
        // opened -- exactly the failure this whole mechanism exists to avoid.
        result.success = waitForMountPoint(result.localPath, kMountVisibleBudgetMs);
        if (!result.success)
            result.error = QStringLiteral("mounted, but %1 did not become usable within %2 ms")
                               .arg(result.localPath)
                               .arg(kMountVisibleBudgetMs);
        return result;
    }

    if (code == 0)
        result.error =
            QStringLiteral("mounted, but could not resolve local path under %1").arg(gvfsRoot());
    else
        result.error = err.isEmpty()
                           ? QStringLiteral("gio mount failed (exit %1)").arg(code)
                           : err;
    return result;
}

bool GvfsMounter::unmount(const QString &uri, int timeoutMs) {
    if (uri.trimmed().isEmpty())
        return false;
    QString out, err;
    const bool ok = runGio({QStringLiteral("mount"), QStringLiteral("-u"), uri}, &out, &err,
                           timeoutMs) == 0;
    if (ok)
        forgetMounts();
    return ok;
}

QString GvfsMounter::parseLocalPath(const QString &output) {
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (!t.startsWith(QStringLiteral("local path:"), Qt::CaseInsensitive))
            continue;
        const QString p = t.mid(t.indexOf(QLatin1Char(':')) + 1).trimmed();
        if (!p.isEmpty())
            return p;
    }
    return QString();
}

QString GvfsMounter::localPathForUri(const QString &uri) {
    // Authoritative: gio translates the URI against the live mount table, which
    // also settles the awkward parts on its own -- an SMB share's letter case, a
    // WebDAV mount's prefix, and whether the mount is still alive at all.
    QString out, err;
    if (runGio({QStringLiteral("info"), uri}, &out, &err, 15000) == 0) {
        const QString path = parseLocalPath(out);
        if (!path.isEmpty())
            return path;
    }

    // Fallback: match the URI against the mount directories that really exist.
    // This is what covers gio being absent or the object not existing yet while
    // its mount does.
    const QString dir = matchMountDir(gvfsRootEntries(), uri);
    if (!dir.isEmpty())
        return gvfsRoot() + QLatin1Char('/') + dir;
    return QString();
}

QString GvfsMounter::localPathFor(const FileProvider *provider, const QString &remotePath,
                                  ResolveFlags flags) {
    if (!provider)
        return QString();
    return localPathFor(provider->remoteLocation(), remotePath, flags);
}

QString GvfsMounter::localPathFor(const RemoteLocation &loc, const QString &remotePath,
                                  ResolveFlags flags) {
    if (!loc.isValid())
        return QString();

    const ResolvedMount mount =
        resolveMount(loc, remotePath, flags.testFlag(MountIfNeeded));
    if (!mount.isValid())
        return QString();

    QString rel;
    if (!relativeUnder(mount.remoteRoot, remotePath, &rel))
        return QString(); // the mount we found does not cover this path

    const QString local =
        rel.isEmpty() ? mount.localRoot : mount.localRoot + QLatin1Char('/') + rel;
    if (flags.testFlag(RequireExists) && !QFileInfo::exists(local))
        return QString();
    return local;
}

bool GvfsMounter::isMounted(const RemoteLocation &loc, const QString &remotePath) {
    if (!loc.isValid())
        return false;
    const QStringList entries = gvfsRootEntries();
    const auto targets = mountTargets(loc, remotePath);
    for (const auto &target : targets) {
        if (!matchMountDir(entries, target.uri).isEmpty())
            return true;
    }
    return false;
}

void GvfsMounter::forgetMounts() {
    QMutexLocker locker(&g_cacheMutex);
    g_cache.clear();
}

QVector<GvfsMounter::MountInfo> GvfsMounter::parseMountList(const QString &output) {
    QVector<MountInfo> mounts;
    // `gio mount -li` prints one line per mount:
    //   Mount(0): NAME -> URI
    static const QRegularExpression re(
        QStringLiteral("^\\s*Mount\\(\\d+\\):\\s*(.*?)\\s*->\\s*(\\S+)\\s*$"));
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto m = re.match(line);
        if (!m.hasMatch())
            continue;
        MountInfo info;
        info.name = m.captured(1).trimmed();
        info.uri = m.captured(2).trimmed();
        const QString dir = gvfsMountDirName(info.uri);
        if (!dir.isEmpty())
            info.localPath = gvfsRoot() + QLatin1Char('/') + dir;
        mounts.append(info);
    }
    return mounts;
}

QVector<GvfsMounter::MountInfo> GvfsMounter::listMounts() {
    QString out, err;
    if (runGio({QStringLiteral("mount"), QStringLiteral("-li")}, &out, &err, 10000) != 0)
        return {};
    return parseMountList(out);
}

QVector<GvfsMounter::NetworkLocation> GvfsMounter::parseNetworkList(const QString &output) {
    QVector<NetworkLocation> locations;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.isEmpty())
            continue;
        NetworkLocation loc;
        // The display name is the first tab/space-separated field; any token
        // containing "://" is taken as the activation URI.
        const QStringList fields =
            t.split(QRegularExpression(QStringLiteral("\\t|\\s{2,}")),
                    Qt::SkipEmptyParts);
        loc.displayName = fields.isEmpty() ? t : fields.first().trimmed();
        for (const QString &f : fields) {
            if (f.contains(QStringLiteral("://"))) {
                loc.uri = f.trimmed();
                break;
            }
        }
        locations.append(loc);
    }
    return locations;
}

QVector<GvfsMounter::NetworkLocation> GvfsMounter::networkLocations() {
    QString out, err;
    if (runGio({QStringLiteral("list"),
                QStringLiteral("-a"),
                QStringLiteral("standard::name,standard::target-uri"),
                QStringLiteral("network:///")},
               &out, &err, 10000) != 0) {
        // Retry with a bare listing in case the attribute form is unsupported.
        if (runGio({QStringLiteral("list"), QStringLiteral("network:///")}, &out,
                   &err, 10000) != 0)
            return {};
    }
    return parseNetworkList(out);
}
