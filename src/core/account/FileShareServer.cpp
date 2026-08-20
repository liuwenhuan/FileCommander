#include "FileShareServer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QLocale>
#include <QMap>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>

namespace {

// A request head longer than this is not a request, it is someone probing.
constexpr int kMaxHeadBytes = 64 * 1024;
// PROPFIND bodies are a few hundred bytes; nothing else we serve has a body we
// have to hold in memory (PUT streams straight to disk).
constexpr int kMaxBufferedBody = 1024 * 1024;
constexpr qint64 kSendChunk = 64 * 1024;
// Stop reading ahead of the socket once this much is queued, so a slow peer
// cannot make us buffer a whole file in RAM.
constexpr qint64 kSendHighWater = 512 * 1024;

QByteArray statusText(int code) {
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    default: return "Error";
    }
}

// RFC 1123 in GMT, in English regardless of the machine's locale -- the client
// parses it back with QLocale(English, UnitedStates).
QByteArray httpDate(const QDateTime &when) {
    static const QLocale en(QLocale::English, QLocale::UnitedStates);
    return en.toString(when.toUTC(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'")).toUtf8();
}

QByteArray xmlEscape(const QString &text) {
    QString out = text;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return out.toUtf8();
}

QByteArray hrefFor(const QString &davPath, bool collection) {
    QStringList encoded;
    for (const QString &segment : davPath.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        encoded << QString::fromUtf8(QUrl::toPercentEncoding(segment));
    QByteArray href = "/" + encoded.join(QLatin1Char('/')).toUtf8();
    if (collection && href != "/")
        href += "/";
    return href;
}

// "/a/b/" -> "/a/b", "" -> "/". Everything below works on this form.
QString cleanDavPath(const QString &raw) {
    QString path = raw;
    const int query = path.indexOf(QLatin1Char('?'));
    if (query >= 0)
        path = path.left(query);
    path = QUrl::fromPercentEncoding(path.toUtf8());
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.isEmpty())
        path = QStringLiteral("/");
    return path;
}

} // namespace

// The listening half. Lives on the server thread, so its shares and tickets are
// touched by one thread only and need no lock.
class ShareWorker : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE void startListening(int port);
    Q_INVOKABLE void stopListening();
    Q_INVOKABLE void setFolders(const QStringList &folders);
    Q_INVOKABLE void addTicket(const QString &ticket, int ttlSeconds);

    bool ticketValid(const QString &ticket);
    QStringList shareNames() const { return m_shares.keys(); }

    // Maps a request path onto a real file. Empty when the path names no share,
    // or when it resolves outside one -- which covers "..", an absolute path
    // smuggled in, and a symlink pointing out of the shared folder, because the
    // check is on the canonical result rather than on the text.
    QString resolve(const QString &davPath) const;

signals:
    void listening(quint16 port);
    void failed(const QString &error);
    void closed();

private:
    void onNewConnection();

    QTcpServer *m_server = nullptr;
    QMap<QString, QString> m_shares;      // share name -> absolute local path
    QHash<QString, QPair<QDateTime, int>> m_tickets;  // ticket -> expiry, TTL
};

// One client connection: an HTTP/1.1 state machine that never blocks. Bodies
// stream to disk and responses stream from it, both with the socket's own
// buffering as the brake.
class ShareConnection : public QObject {
    Q_OBJECT

public:
    ShareConnection(QTcpSocket *socket, ShareWorker *worker)
        : QObject(socket), m_socket(socket), m_worker(worker) {
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(m_socket, &QTcpSocket::readyRead, this, &ShareConnection::onReadyRead);
        connect(m_socket, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        connect(m_socket, &QTcpSocket::disconnected, m_socket, &QObject::deleteLater);
    }

    ~ShareConnection() override {
        delete m_upload;
        delete m_download;
    }

private:
    enum class Phase { Head, Body, Sending };

    void onReadyRead() {
        m_buffer += m_socket->readAll();
        process();
    }

    void process() {
        while (!m_dead) {
            if (m_phase == Phase::Head) {
                const int end = m_buffer.indexOf("\r\n\r\n");
                if (end < 0) {
                    if (m_buffer.size() > kMaxHeadBytes)
                        respond(400);
                    return;
                }
                const QByteArray head = m_buffer.left(end);
                m_buffer.remove(0, end + 4);
                if (!parseHead(head))
                    return;
                if (!beginRequest())
                    return;
            } else if (m_phase == Phase::Body) {
                if (!consumeBody())
                    return;
                finishBody();
            } else {
                return; // a response is streaming; process() resumes when it ends
            }
        }
    }

    bool parseHead(const QByteArray &head) {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
        if (request.size() < 2) {
            respond(400);
            return false;
        }
        m_method = request.at(0).toUpper();
        m_target = request.at(1);
        m_keepAlive = !lines.value(0).contains("HTTP/1.0");
        m_headers.clear();
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon > 0)
                m_headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
        }
        if (m_headers.value("connection").toLower().contains("close"))
            m_keepAlive = false;
        return true;
    }

    // Decides what the request needs before its body arrives: rejects it, or
    // opens the upload file, or dispatches it right away.
    bool beginRequest() {
        m_chunked = m_headers.value("transfer-encoding").toLower().contains("chunked");
        m_bodyRemaining = m_chunked ? 0 : m_headers.value("content-length").toLongLong();
        m_chunkRemaining = 0;
        m_needChunkCrlf = false;
        m_chunkDone = false;
        m_bodyBuffer.clear();

        if (!authorised()) {
            // The body (if any) is still coming, so the stream is out of step:
            // answer and hang up. libcurl reconnects with credentials, and for
            // a sizeable upload it is still waiting on our 100-continue.
            m_keepAlive = false;
            respond(401, QByteArray(), "text/plain",
                    "WWW-Authenticate: Basic realm=\"FileCommander\"\r\n");
            return false;
        }

        if (m_headers.value("expect").toLower().contains("100-continue"))
            m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");

        if (m_method == "PUT") {
            const QString local = m_worker->resolve(cleanDavPath(QString::fromUtf8(m_target)));
            if (local.isEmpty()) {
                m_keepAlive = false;
                respond(403);
                return false;
            }
            // A plain PUT replaces the whole resource. A resumed one says where
            // its body belongs with Content-Range, and then everything before
            // that offset must survive untouched -- that is the whole point of
            // resuming, and truncating here would silently restart from zero.
            const qint64 have = QFileInfo(local).size();
            qint64 at = 0;
            const QByteArray range = m_headers.value("content-range");
            if (range.startsWith("bytes ")) {
                bool ok = false;
                at = range.mid(6).split('/').value(0).split('-').value(0).trimmed().toLongLong(&ok);
                // Nothing to continue from past what we actually hold: refusing
                // is the only answer that cannot corrupt the file.
                if (!ok || at < 0 || at > have) {
                    m_keepAlive = false;
                    respond(416, QByteArray(), "text/plain",
                            "Content-Range: bytes */" + QByteArray::number(have) + "\r\n");
                    return false;
                }
            }
            m_upload = new QFile(local);
            // ReadWrite, not WriteOnly: WriteOnly on its own truncates the file
            // as it opens it, which is precisely what a resume must not do.
            const QIODevice::OpenMode mode =
                at > 0 ? QIODevice::ReadWrite : QIODevice::WriteOnly | QIODevice::Truncate;
            // resize() drops any stale tail beyond the resume point, so the
            // result is the same whether or not the last attempt overshot.
            if (!m_upload->open(mode) || (at > 0 && (!m_upload->resize(at) || !m_upload->seek(at)))) {
                delete m_upload;
                m_upload = nullptr;
                m_keepAlive = false;
                respond(409);
                return false;
            }
            m_uploadExisted = have > 0;
            m_phase = Phase::Body;
            return true;
        }

        if (m_chunked || m_bodyRemaining > 0) {
            if (m_bodyRemaining > kMaxBufferedBody) {
                m_keepAlive = false;
                respond(413);
                return false;
            }
            m_phase = Phase::Body;
            return true;
        }
        dispatch();
        return !m_dead;
    }

    void sinkWrite(const QByteArray &data) {
        if (m_upload)
            m_upload->write(data);
        else
            m_bodyBuffer += data;
    }

    // True once the whole body has been consumed. Handles both framings: a
    // Content-Length body, and the chunked one libcurl falls back to whenever
    // the caller could not tell it the size up front.
    bool consumeBody() {
        if (m_chunked) {
            while (true) {
                if (m_chunkRemaining > 0) {
                    const qint64 take = qMin<qint64>(m_chunkRemaining, m_buffer.size());
                    if (take > 0) {
                        sinkWrite(m_buffer.left(static_cast<int>(take)));
                        m_buffer.remove(0, static_cast<int>(take));
                        m_chunkRemaining -= take;
                    }
                    if (m_chunkRemaining > 0)
                        return false;
                    m_needChunkCrlf = true;
                }
                if (m_needChunkCrlf) {
                    if (m_buffer.size() < 2)
                        return false;
                    m_buffer.remove(0, 2);
                    m_needChunkCrlf = false;
                }
                if (m_chunkDone)
                    return true;
                const int eol = m_buffer.indexOf("\r\n");
                if (eol < 0)
                    return false;
                const QByteArray line = m_buffer.left(eol);
                m_buffer.remove(0, eol + 2);
                bool ok = false;
                const qint64 size = line.split(';').value(0).trimmed().toLongLong(&ok, 16);
                if (!ok || size < 0) {
                    m_keepAlive = false;
                    respond(400);
                    return false;
                }
                if (size == 0) {
                    // The last chunk; one more CRLF closes the (empty) trailer.
                    m_chunkDone = true;
                    m_needChunkCrlf = true;
                    continue;
                }
                m_chunkRemaining = size;
                if (!m_upload && m_bodyBuffer.size() + size > kMaxBufferedBody) {
                    m_keepAlive = false;
                    respond(413);
                    return false;
                }
            }
        }
        if (m_bodyRemaining > 0) {
            const qint64 take = qMin<qint64>(m_bodyRemaining, m_buffer.size());
            if (take > 0) {
                sinkWrite(m_buffer.left(static_cast<int>(take)));
                m_buffer.remove(0, static_cast<int>(take));
                m_bodyRemaining -= take;
            }
        }
        return m_bodyRemaining == 0;
    }

    void finishBody() {
        if (m_upload) {
            const bool ok = m_upload->flush();
            m_upload->close();
            delete m_upload;
            m_upload = nullptr;
            respond(ok ? (m_uploadExisted ? 204 : 201) : 500);
            return;
        }
        dispatch();
    }

    bool authorised() {
        const QByteArray auth = m_headers.value("authorization");
        if (!auth.startsWith("Basic "))
            return false;
        const QByteArray decoded = QByteArray::fromBase64(auth.mid(6).trimmed());
        const int colon = decoded.indexOf(':');
        if (colon < 0)
            return false;
        return m_worker->ticketValid(QString::fromUtf8(decoded.mid(colon + 1)));
    }

    void dispatch() {
        const QString path = cleanDavPath(QString::fromUtf8(m_target));
        if (m_method == "OPTIONS") {
            respond(200, QByteArray(), "text/plain",
                    "DAV: 1,2\r\nAllow: OPTIONS, PROPFIND, GET, HEAD, PUT, MKCOL, DELETE, MOVE\r\n");
        } else if (m_method == "PROPFIND") {
            doPropfind(path);
        } else if (m_method == "GET" || m_method == "HEAD") {
            doGet(path, m_method == "HEAD");
        } else if (m_method == "MKCOL") {
            doMkcol(path);
        } else if (m_method == "DELETE") {
            doDelete(path);
        } else if (m_method == "MOVE") {
            doMove(path);
        } else {
            respond(405);
        }
    }

    // One <response> element. `davPath` is what the client will use as a path,
    // so it must be the request-relative form, not the local one.
    static QByteArray propEntry(const QString &davPath, const QString &name,
                                const QFileInfo &info, bool collection) {
        QByteArray out = "<D:response><D:href>" + hrefFor(davPath, collection) + "</D:href>";
        out += "<D:propstat><D:prop>";
        out += "<D:displayname>" + xmlEscape(name) + "</D:displayname>";
        out += collection ? "<D:resourcetype><D:collection/></D:resourcetype>"
                          : "<D:resourcetype/>";
        if (!collection)
            out += "<D:getcontentlength>" + QByteArray::number(info.size()) +
                   "</D:getcontentlength>";
        const QDateTime modified =
            info.exists() ? info.lastModified() : QDateTime::currentDateTimeUtc();
        out += "<D:getlastmodified>" + httpDate(modified) + "</D:getlastmodified>";
        out += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
        return out;
    }

    void doPropfind(const QString &path) {
        const bool deep = m_headers.value("depth", "1") != "0";
        const bool isRoot = path == QLatin1String("/");

        QByteArray body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<D:multistatus xmlns:D=\"DAV:\">";
        if (isRoot) {
            // The virtual root: the shared folders, and nothing else. It is not
            // a directory on disk, so it is built rather than listed.
            if (!deep)
                body += propEntry(QStringLiteral("/"), QStringLiteral("FileCommander"),
                                  QFileInfo(), true);
            else
                for (const QString &name : m_worker->shareNames())
                    body += propEntry(QLatin1Char('/') + name, name,
                                      QFileInfo(m_worker->resolve(QLatin1Char('/') + name)), true);
        } else {
            const QString local = m_worker->resolve(path);
            const QFileInfo info(local);
            if (local.isEmpty() || !info.exists()) {
                respond(404);
                return;
            }
            if (!deep) {
                body += propEntry(path, info.fileName(), info, info.isDir());
            } else if (info.isDir()) {
                // The collection's own entry is deliberately left out: the
                // client only skips a self-reference whose path matches the one
                // it asked for, and it asks with the trailing slash stripped.
                const QFileInfoList children = QDir(local).entryInfoList(
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
                for (const QFileInfo &child : children)
                    body += propEntry(path + QLatin1Char('/') + child.fileName(),
                                      child.fileName(), child, child.isDir());
            } else {
                body += propEntry(path, info.fileName(), info, false);
            }
        }
        body += "</D:multistatus>";
        respond(207, body, "application/xml; charset=utf-8");
    }

    void doGet(const QString &path, bool headOnly) {
        const QString local = m_worker->resolve(path);
        const QFileInfo info(local);
        if (local.isEmpty() || !info.exists()) {
            respond(404);
            return;
        }
        if (info.isDir()) {
            respond(405);
            return;
        }
        auto *file = new QFile(local);
        if (!file->open(QIODevice::ReadOnly)) {
            delete file;
            respond(403);
            return;
        }

        const qint64 total = file->size();
        qint64 offset = 0;
        qint64 length = total;
        int status = 200;
        QByteArray extra; // Accept-Ranges goes out on every response, see sendHead()
        const QByteArray range = m_headers.value("range");
        if (range.startsWith("bytes=")) {
            const QList<QByteArray> bounds = range.mid(6).split('-');
            offset = bounds.value(0).trimmed().toLongLong();
            const QByteArray last = bounds.value(1).trimmed();
            const qint64 end = last.isEmpty() ? total - 1 : last.toLongLong();
            if (offset >= total || end < offset) {
                delete file;
                respond(416, QByteArray(), "text/plain",
                        "Content-Range: bytes */" + QByteArray::number(total) + "\r\n");
                return;
            }
            length = end - offset + 1;
            status = 206;
            extra += "Content-Range: bytes " + QByteArray::number(offset) + "-" +
                     QByteArray::number(end) + "/" + QByteArray::number(total) + "\r\n";
            file->seek(offset);
        }
        extra += "Last-Modified: " + httpDate(info.lastModified()) + "\r\n";

        sendHead(status, length, "application/octet-stream", extra);
        if (headOnly) {
            delete file;
            finishResponse();
            return;
        }
        m_download = file;
        m_sendRemaining = length;
        m_phase = Phase::Sending;
        pump();
    }

    void pump() {
        if (!m_download || m_dead)
            return;
        while (m_sendRemaining > 0 && m_socket->bytesToWrite() < kSendHighWater) {
            const QByteArray chunk = m_download->read(qMin(kSendChunk, m_sendRemaining));
            if (chunk.isEmpty()) {
                // Truncated under us: the declared length can no longer be met,
                // so the only honest thing left is to drop the connection.
                m_sendRemaining = 0;
                m_keepAlive = false;
                break;
            }
            m_socket->write(chunk);
            m_sendRemaining -= chunk.size();
        }
        if (m_sendRemaining > 0)
            return;
        delete m_download;
        m_download = nullptr;
        finishResponse();
        process();
    }

    void doMkcol(const QString &path) {
        const QString local = m_worker->resolve(path);
        if (local.isEmpty()) {
            respond(403);
            return;
        }
        const QFileInfo info(local);
        if (info.exists()) {
            respond(405);
            return;
        }
        if (!QFileInfo(info.absolutePath()).isDir()) {
            respond(409); // MKCOL does not create intermediate collections
            return;
        }
        respond(QDir().mkdir(local) ? 201 : 403);
    }

    void doDelete(const QString &path) {
        const QString local = m_worker->resolve(path);
        const QFileInfo info(local);
        if (local.isEmpty() || !info.exists()) {
            respond(404);
            return;
        }
        const bool ok = info.isDir() ? QDir(local).removeRecursively() : QFile::remove(local);
        respond(ok ? 204 : 403);
    }

    void doMove(const QString &path) {
        const QString from = m_worker->resolve(path);
        if (from.isEmpty() || !QFileInfo::exists(from)) {
            respond(404);
            return;
        }
        const QUrl destination(QString::fromUtf8(m_headers.value("destination")));
        const QString to = m_worker->resolve(cleanDavPath(destination.path()));
        if (to.isEmpty()) {
            respond(403);
            return;
        }
        const bool occupied = QFileInfo::exists(to);
        if (occupied && m_headers.value("overwrite").toUpper() == "F") {
            respond(412);
            return;
        }
        if (occupied) {
            const QFileInfo target(to);
            const bool cleared =
                target.isDir() ? QDir(to).removeRecursively() : QFile::remove(to);
            if (!cleared) {
                respond(403);
                return;
            }
        }
        // A failed rename must leave the source alone -- the client's moveTo()
        // contract is that it can always fall back to copy + delete.
        respond(QFile::rename(from, to) ? (occupied ? 204 : 201) : 403);
    }

    void sendHead(int status, qint64 contentLength, const QByteArray &contentType,
                  const QByteArray &extra) {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " " + statusText(status) +
                          "\r\n";
        head += "Date: " + httpDate(QDateTime::currentDateTimeUtc()) + "\r\n";
        // Resuming a PUT is not something WebDAV standardises, so a client may
        // not simply assume it: a server that ignored Content-Range would store
        // the tail as the whole file. Say so explicitly, on every response, and
        // the peer's provider can turn resume on for this link only.
        head += "Accept-Ranges: bytes\r\nX-FileCommander-Put-Range: bytes\r\n";
        if (contentLength >= 0) {
            head += "Content-Length: " + QByteArray::number(contentLength) + "\r\n";
            if (contentLength > 0)
                head += "Content-Type: " + contentType + "\r\n";
        }
        head += extra;
        head += m_keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        head += "\r\n";
        m_socket->write(head);
    }

    void respond(int status, const QByteArray &body = QByteArray(),
                 const QByteArray &contentType = "text/plain",
                 const QByteArray &extra = QByteArray()) {
        sendHead(status, body.size(), contentType, extra);
        if (!body.isEmpty())
            m_socket->write(body);
        finishResponse();
    }

    void finishResponse() {
        if (!m_keepAlive) {
            m_dead = true;
            m_socket->disconnectFromHost();
            return;
        }
        m_phase = Phase::Head;
        m_method.clear();
        m_target.clear();
        m_bodyBuffer.clear();
    }

    QTcpSocket *m_socket;
    ShareWorker *m_worker;
    QByteArray m_buffer;
    Phase m_phase = Phase::Head;
    bool m_dead = false;

    QByteArray m_method;
    QByteArray m_target;
    QHash<QByteArray, QByteArray> m_headers;
    bool m_keepAlive = true;

    bool m_chunked = false;
    qint64 m_bodyRemaining = 0;
    qint64 m_chunkRemaining = 0;
    bool m_needChunkCrlf = false;
    bool m_chunkDone = false;
    QByteArray m_bodyBuffer;

    QFile *m_upload = nullptr;
    bool m_uploadExisted = false;
    QFile *m_download = nullptr;
    qint64 m_sendRemaining = 0;
};

void ShareWorker::startListening(int port) {
    if (m_server)
        return;
    m_server = new QTcpServer(this);
    // Peers reach this port directly, over the LAN or through the relay -- never
    // through the user's proxy. Without this, an $all_proxy in the environment
    // makes QTcpServer try to bind through SOCKS and listen() simply fails.
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &ShareWorker::onNewConnection);
    if (!m_server->listen(QHostAddress::Any, static_cast<quint16>(port))) {
        const QString reason = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        emit failed(reason);
        return;
    }
    emit listening(m_server->serverPort());
}

void ShareWorker::stopListening() {
    if (!m_server)
        return;
    delete m_server; // closes the listener and every connection parented to it
    m_server = nullptr;
    m_tickets.clear();
    emit closed();
}

void ShareWorker::setFolders(const QStringList &folders) {
    m_shares.clear();
    for (const QString &folder : folders) {
        const QString canonical = QFileInfo(folder).canonicalFilePath();
        if (canonical.isEmpty())
            continue; // gone, or not a real path
        QString name = QFileInfo(canonical).fileName();
        if (name.isEmpty())
            name = QStringLiteral("root");
        // Two shared folders can end in the same basename; both still have to
        // be reachable, so the second one gets a suffix.
        QString unique = name;
        for (int n = 2; m_shares.contains(unique); ++n)
            unique = name + QStringLiteral(" (%1)").arg(n);
        m_shares.insert(unique, canonical);
    }
}

void ShareWorker::addTicket(const QString &ticket, int ttlSeconds) {
    if (ticket.isEmpty())
        return;
    // No floor on the TTL: a ticket handed over already expired is worthless
    // by construction, and clamping it up to a second would make it briefly
    // usable instead.
    m_tickets.insert(ticket, {QDateTime::currentDateTimeUtc().addSecs(ttlSeconds), ttlSeconds});
}

bool ShareWorker::ticketValid(const QString &ticket) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_tickets.begin(); it != m_tickets.end();) {
        if (it.value().first <= now)
            it = m_tickets.erase(it);
        else
            ++it;
    }
    if (ticket.isEmpty())
        return false;
    auto it = m_tickets.find(ticket);
    if (it == m_tickets.end())
        return false;
    // Sliding: the TTL measures idleness, not total lifetime. A fixed expiry
    // would 401 in the middle of a browse the user is still using.
    it.value().first = now.addSecs(it.value().second);
    return true;
}

QString ShareWorker::resolve(const QString &davPath) const {
    const QStringList segments = davPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty())
        return QString(); // the virtual root is not a file
    const QString root = m_shares.value(segments.first());
    if (root.isEmpty())
        return QString();
    const QString candidate =
        segments.size() == 1 ? root : root + QLatin1Char('/') + segments.mid(1).join(QLatin1Char('/'));

    // Judge the canonical result, not the text: that is what catches a symlink
    // pointing out of the share as well as a "..".
    const QFileInfo info(candidate);
    QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        // Does not exist yet (a PUT or MKCOL target): its parent must.
        canonical = QFileInfo(info.absolutePath()).canonicalFilePath();
        if (canonical.isEmpty())
            return QString();
        canonical += QLatin1Char('/') + info.fileName();
    }
    if (canonical != root && !canonical.startsWith(root + QLatin1Char('/')))
        return QString();
    return canonical;
}

void ShareWorker::onNewConnection() {
    while (QTcpSocket *socket = m_server->nextPendingConnection())
        new ShareConnection(socket, this);
}

FileShareServer::FileShareServer(QObject *parent)
    : QObject(parent), m_thread(new QThread(this)), m_worker(new ShareWorker) {
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, SIGNAL(listening(quint16)), this, SIGNAL(started(quint16)));
    connect(m_worker, SIGNAL(failed(QString)), this, SIGNAL(failed(QString)));
    connect(m_worker, SIGNAL(closed()), this, SIGNAL(stopped()));
    connect(m_worker, SIGNAL(listening(quint16)), this, SLOT(rememberPort(quint16)));
    connect(m_worker, SIGNAL(closed()), this, SLOT(forgetPort()));
    m_thread->start();
}

FileShareServer::~FileShareServer() {
    QMetaObject::invokeMethod(m_worker, "stopListening", Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

void FileShareServer::setSharedFolders(const QStringList &folders) {
    QMetaObject::invokeMethod(m_worker, "setFolders", Qt::QueuedConnection,
                              Q_ARG(QStringList, folders));
}

void FileShareServer::addTicket(const QString &ticket, int ttlSeconds) {
    QMetaObject::invokeMethod(m_worker, "addTicket", Qt::QueuedConnection,
                              Q_ARG(QString, ticket), Q_ARG(int, ttlSeconds));
}

void FileShareServer::start(quint16 port) {
    QMetaObject::invokeMethod(m_worker, "startListening", Qt::QueuedConnection,
                              Q_ARG(int, static_cast<int>(port)));
}

void FileShareServer::stop() {
    QMetaObject::invokeMethod(m_worker, "stopListening", Qt::QueuedConnection);
}

void FileShareServer::rememberPort(quint16 port) { m_port = port; }

void FileShareServer::forgetPort() { m_port = 0; }

#include "FileShareServer.moc"
