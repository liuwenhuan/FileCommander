#include "GvfsMounter.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <unistd.h> // getuid()

namespace {

// Run `gio <args>`, feeding `stdinData` if provided. Returns the process exit
// code (or -1 if it failed to start / timed out); fills stdout/stderr.
int runGio(const QStringList &args, QString *stdOut, QString *stdErr, int timeoutMs,
           const QByteArray &stdinData = QByteArray()) {
    QProcess proc;
    proc.start(QStringLiteral("gio"), args);
    if (!proc.waitForStarted(timeoutMs)) {
        if (stdErr)
            *stdErr = QStringLiteral("failed to start `gio` — is gvfs installed?");
        return -1;
    }
    if (!stdinData.isEmpty()) {
        proc.write(stdinData);
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

QString GvfsMounter::gvfsRoot() {
    return QStringLiteral("/run/user/%1/gvfs").arg(getuid());
}

QString GvfsMounter::gvfsMountDirName(const QString &uri) {
    const QUrl url(uri);
    if (!url.isValid() || url.host().isEmpty())
        return QString();

    const QString sch = url.scheme();
    const QString host = url.host();
    const QString user = url.userName();
    const int port = url.port();

    QStringList parts;
    if (sch == QLatin1String("smb")) {
        // smb://server/share/... -> smb-share:server=SERVER,share=SHARE
        const QStringList segs =
            url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        parts << QStringLiteral("server=%1").arg(host);
        if (!segs.isEmpty())
            parts << QStringLiteral("share=%1").arg(segs.first());
        if (!user.isEmpty())
            parts << QStringLiteral("user=%1").arg(user);
        return QStringLiteral("smb-share:") + parts.join(QLatin1Char(','));
    }

    if (sch == QLatin1String("dav") || sch == QLatin1String("davs")) {
        parts << QStringLiteral("host=%1").arg(host);
        if (port > 0)
            parts << QStringLiteral("port=%1").arg(port);
        parts << QStringLiteral("ssl=%1").arg(sch == QLatin1String("davs")
                                                  ? QStringLiteral("true")
                                                  : QStringLiteral("false"));
        return QStringLiteral("dav:") + parts.join(QLatin1Char(','));
    }

    // sftp / ftp: <scheme>:host=HOST[,port=N][,user=U]
    parts << QStringLiteral("host=%1").arg(host);
    if (port > 0)
        parts << QStringLiteral("port=%1").arg(port);
    if (!user.isEmpty())
        parts << QStringLiteral("user=%1").arg(user);
    return sch + QLatin1Char(':') + parts.join(QLatin1Char(','));
}

GvfsMounter::MountResult GvfsMounter::mount(const QString &uri, const QString &password,
                                            int timeoutMs) {
    MountResult result;
    result.uri = uri;

    if (uri.trimmed().isEmpty()) {
        result.error = QStringLiteral("empty URI");
        return result;
    }

    QString out, err;
    // TODO(phase-2): replace stdin password feed with libsecret-backed
    // credential lookup so passwords never transit argv/env/stdin in the clear.
    const QByteArray stdinData =
        password.isEmpty() ? QByteArray() : password.toUtf8();
    const int code = runGio({QStringLiteral("mount"), uri}, &out, &err, timeoutMs,
                            stdinData);

    // Already-mounted is reported by gio as an error but is success for us.
    const bool alreadyMounted =
        err.contains(QStringLiteral("already mounted"), Qt::CaseInsensitive);

    if (code == 0 || alreadyMounted) {
        result.localPath = localPathForUri(uri);
        result.success = !result.localPath.isEmpty();
        if (!result.success)
            result.error = QStringLiteral(
                "mounted, but could not resolve local path under %1")
                               .arg(gvfsRoot());
        return result;
    }

    result.error = err.isEmpty()
                       ? QStringLiteral("gio mount failed (exit %1)").arg(code)
                       : err;
    return result;
}

bool GvfsMounter::unmount(const QString &uri, int timeoutMs) {
    if (uri.trimmed().isEmpty())
        return false;
    QString out, err;
    return runGio({QStringLiteral("mount"), QStringLiteral("-u"), uri}, &out, &err,
                  timeoutMs) == 0;
}

QString GvfsMounter::localPathForUri(const QString &uri) {
    // Authoritative: ask gio for the object's local path.
    QString out, err;
    if (runGio({QStringLiteral("info"), uri}, &out, &err, 10000) == 0) {
        const QStringList lines = out.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString t = line.trimmed();
            if (t.startsWith(QStringLiteral("local path:"), Qt::CaseInsensitive)) {
                const QString p =
                    t.mid(t.indexOf(QLatin1Char(':')) + 1).trimmed();
                if (!p.isEmpty())
                    return p;
            }
        }
    }

    // Fallback: construct the conventional directory name and check it exists.
    const QString dir = gvfsMountDirName(uri);
    if (!dir.isEmpty()) {
        const QString candidate = gvfsRoot() + QLatin1Char('/') + dir;
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
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
