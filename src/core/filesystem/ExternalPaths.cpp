#include "ExternalPaths.h"

#include <QMimeData>

#if FILECOMMANDER_HAS_LINUX_INTEGRATION
#include "GvfsMounter.h"
#endif

namespace fc {

QByteArray encodeInternalPaths(const QStringList &paths, bool cut) {
    QByteArray data = cut ? "cut\n" : "copy\n";
    for (const QString &path : paths)
        data += path.toUtf8() + "\n";
    return data;
}

QStringList decodeInternalPaths(const QByteArray &data, bool *cut) {
    if (cut)
        *cut = false;
    QStringList paths;
    const QList<QByteArray> lines = data.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (i == 0) {
            if (cut)
                *cut = (line == "cut");
            continue;
        }
        if (!line.isEmpty())
            paths.append(QString::fromUtf8(line));
    }
    return paths;
}

QStringList incomingPaths(const QMimeData *mime, bool *cut) {
    if (cut)
        *cut = false;
    if (!mime)
        return {};
    if (mime->hasFormat(QLatin1String(kInternalPathsMime))) {
        const QStringList paths =
            decodeInternalPaths(mime->data(QLatin1String(kInternalPathsMime)), cut);
        if (!paths.isEmpty())
            return paths;
    }
    QStringList paths;
    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        // Only local files: a remote URI from another application names a server
        // this process has no connection to, so there is nothing to copy from.
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    return paths;
}

bool hasIncomingPaths(const QMimeData *mime) {
    return mime && (mime->hasFormat(QLatin1String(kInternalPathsMime)) || mime->hasUrls());
}

QUrl externalUrlFor(bool localFilesystem, const RemoteLocation &loc, const QString &path,
                    const QString &mountedLocalPath) {
    if (path.isEmpty())
        return {};
    // A local backend's paths already ARE paths on this machine.
    if (localFilesystem)
        return QUrl::fromLocalFile(path);
    // A live mount gives the server's file a real name here, which every
    // program understands and opens directly off the server.
    if (!mountedLocalPath.isEmpty())
        return QUrl::fromLocalFile(mountedLocalPath);
    // No server to name: an archive entry (or a backend that hasn't been taught
    // remoteLocation()). Nothing outside this process can reach it, and saying
    // so by omission is the only honest answer.
    if (!loc.isValid())
        return {};
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    const QString uri = GvfsMounter::uriForPath(loc, path);
    return uri.isEmpty() ? QUrl() : QUrl(uri);
#else
    QUrl url;
    url.setScheme(loc.scheme);
    url.setHost(loc.host);
    if (loc.port > 0)
        url.setPort(loc.port);
    if (!loc.user.isEmpty())
        url.setUserName(loc.user);
    url.setPath(path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path);
    return url;
#endif
}

void setPathPayload(QMimeData *mime, FileProvider *provider, const QStringList &paths, bool cut) {
    if (!mime)
        return;
    mime->setData(QLatin1String(kInternalPathsMime), encodeInternalPaths(paths, cut));
    const QList<QUrl> urls = externalUrlsFor(provider, paths);
    // Deliberately not set at all when empty: an empty text/uri-list still
    // advertises "I am a file drag" to the other application, which then accepts
    // the drop and does nothing.
    if (!urls.isEmpty())
        mime->setUrls(urls);
}

QList<QUrl> externalUrlsFor(FileProvider *provider, const QStringList &paths) {
    QList<QUrl> urls;
    if (!provider)
        return urls;

    const bool local = provider->isLocalFilesystem();
    const RemoteLocation loc = local ? RemoteLocation() : provider->remoteLocation();

    // One unmounted answer settles it for the whole batch: a selection comes out
    // of a single directory, so every path in it is served by the same mount.
    // Being wrong here only downgrades the rest to the URI form, which is still
    // honest -- while being right saves a subprocess per file on a cold
    // connection, on the GUI thread, mid-drag.
    bool mountUnavailable = false;

    for (const QString &path : paths) {
        QString mounted;
        if (!local && loc.isValid() && !mountUnavailable) {
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
            mounted = GvfsMounter::localPathFor(loc, path, GvfsMounter::ResolveFlags());
            if (mounted.isEmpty())
                mountUnavailable = true;
#else
            mountUnavailable = true;
#endif
        }
        const QUrl url = externalUrlFor(local, loc, path, mounted);
        if (!url.isEmpty())
            urls.append(url);
    }
    return urls;
}

} // namespace fc
