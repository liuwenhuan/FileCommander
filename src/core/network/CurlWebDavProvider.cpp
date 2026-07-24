#include "CurlWebDavProvider.h"

#include <curl/curl.h>

#include <cstring>
#include <thread>

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QMutexLocker>
#include <QUrl>
#include <QWaitCondition>
#include <QXmlStreamReader>

#include "FileInfo.h"

namespace {

void ensureCurlGlobalInit() {
    static const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    Q_UNUSED(rc);
}

size_t appendCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<QByteArray *>(userdata);
    buf->append(ptr, static_cast<int>(size * nmemb));
    return size * nmemb;
}

size_t discardCallback(char * /*ptr*/, size_t size, size_t nmemb, void * /*userdata*/) {
    return size * nmemb;
}

constexpr qint64 kPipeCapacity = 512 * 1024;

struct WebDavTransferState {
    QMutex mutex;
    QWaitCondition cond;
    QByteArray buffer;
    bool curlFinished = false;
    bool noMoreInput = false;
    bool aborted = false;
    bool curlOk = false;
};

size_t downloadWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *state = static_cast<WebDavTransferState *>(userdata);
    const size_t total = size * nmemb;
    QMutexLocker locker(&state->mutex);
    while (state->buffer.size() >= kPipeCapacity && !state->aborted)
        state->cond.wait(&state->mutex);
    if (state->aborted)
        return 0;
    state->buffer.append(ptr, static_cast<int>(total));
    state->cond.wakeAll();
    return total;
}

size_t uploadReadCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *state = static_cast<WebDavTransferState *>(userdata);
    const size_t want = size * nmemb;
    QMutexLocker locker(&state->mutex);
    while (state->buffer.isEmpty() && !state->noMoreInput && !state->aborted)
        state->cond.wait(&state->mutex);
    if (state->aborted)
        return CURL_READFUNC_ABORT;
    if (state->buffer.isEmpty() && state->noMoreInput)
        return 0;
    const int n = static_cast<int>(qMin<qint64>(static_cast<qint64>(want), state->buffer.size()));
    std::memcpy(ptr, state->buffer.constData(), static_cast<size_t>(n));
    state->buffer.remove(0, n);
    state->cond.wakeAll();
    return static_cast<size_t>(n);
}

int progressCallback(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *state = static_cast<WebDavTransferState *>(userdata);
    QMutexLocker locker(&state->mutex);
    return state->aborted ? 1 : 0;
}

QString davUrl(const QString &host, int port, bool useHttps, const QString &path,
               bool isDirectory) {
    QStringList rawSegs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList encSegs;
    encSegs.reserve(rawSegs.size());
    for (const QString &seg : rawSegs)
        encSegs << QString::fromUtf8(QUrl::toPercentEncoding(seg));

    QString url = useHttps ? QStringLiteral("https://") : QStringLiteral("http://");
    url += host;
    const int defaultPort = useHttps ? 443 : 80;
    if (port > 0 && port != defaultPort)
        url += QLatin1Char(':') + QString::number(port);
    url += QLatin1Char('/');
    url += encSegs.join(QLatin1Char('/'));
    if (isDirectory && !url.endsWith(QLatin1Char('/')))
        url += QLatin1Char('/');
    return url;
}

} // namespace

struct WebDavHandle : public FileHandle {
    enum class Mode { Read, Write };

    Mode mode = Mode::Read;
    QString path;
    QString host;
    int port = 80;
    QString user;
    QString password;
    bool useHttps = false;
    qint64 resumeOffset = 0;
    bool started = false;
    qint64 cachedSize = -1;
    int timeoutMs = 12000; // connect-phase timeout for the transfer

    CURL *curl = nullptr;
    std::thread worker;
    std::shared_ptr<WebDavTransferState> state = std::make_shared<WebDavTransferState>();

    ~WebDavHandle() override {
        if (worker.joinable()) {
            {
                QMutexLocker locker(&state->mutex);
                state->aborted = true;
                state->noMoreInput = true;
                state->cond.wakeAll();
            }
            worker.join();
        }
        if (curl)
            curl_easy_cleanup(curl);
    }
};

namespace {

void startWebDavTransfer(WebDavHandle *h) {
    h->curl = curl_easy_init();
    auto state = h->state;
    if (!h->curl) {
        QMutexLocker locker(&state->mutex);
        state->curlFinished = true;
        state->curlOk = false;
        h->started = true;
        return;
    }

    const QString url = davUrl(h->host, h->port, h->useHttps, h->path, false);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(h->curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(h->curl, CURLOPT_NOSIGNAL, 1L);
    // Bound the connect phase only. No total CURLOPT_TIMEOUT_MS: a bulk GET/PUT
    // may legitimately run longer than the connect timeout, and a stalled
    // transfer is already abortable via the progress callback / closeHandle().
    curl_easy_setopt(h->curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(h->timeoutMs));
    curl_easy_setopt(h->curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
    curl_easy_setopt(h->curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (!h->user.isEmpty()) {
        const QByteArray userUtf8 = h->user.toUtf8();
        const QByteArray passUtf8 = h->password.toUtf8();
        curl_easy_setopt(h->curl, CURLOPT_USERNAME, userUtf8.constData());
        curl_easy_setopt(h->curl, CURLOPT_PASSWORD, passUtf8.constData());
    }
    curl_easy_setopt(h->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h->curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(h->curl, CURLOPT_XFERINFODATA, state.get());

    if (h->mode == WebDavHandle::Mode::Read) {
        curl_easy_setopt(h->curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(h->curl, CURLOPT_WRITEFUNCTION, downloadWriteCallback);
        curl_easy_setopt(h->curl, CURLOPT_WRITEDATA, state.get());
        if (h->resumeOffset > 0) {
            const QByteArray range = (QString::number(h->resumeOffset) + QStringLiteral("-")).toUtf8();
            // CURLOPT_RANGE keeps the range string alive on the handle until
            // the perform() call below reads it; the QByteArray must outlive
            // the request, so leak it via the handle's stored path trick is
            // avoided by copying into a curl-owned buffer immediately.
            curl_easy_setopt(h->curl, CURLOPT_RANGE, range.constData());
        }
    } else {
        curl_easy_setopt(h->curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h->curl, CURLOPT_READFUNCTION, uploadReadCallback);
        curl_easy_setopt(h->curl, CURLOPT_READDATA, state.get());
        // CURLOPT_UPLOAD alone is sufficient for an HTTP PUT (the deprecated
        // CURLOPT_PUT option is redundant with it and only needed pre-7.12.1).
        // No CURLOPT_INFILESIZE_LARGE: the total size isn't known up front by
        // this streaming API, so curl sends the upload chunked, which every
        // mainstream WebDAV server (Apache mod_dav, nginx-dav, Nextcloud,
        // etc.) accepts for PUT.
    }

    CURL *curl = h->curl;
    h->worker = std::thread([curl, state]() {
        const CURLcode res = curl_easy_perform(curl);
        QMutexLocker locker(&state->mutex);
        state->curlFinished = true;
        state->curlOk = (res == CURLE_OK);
        state->cond.wakeAll();
    });
    h->started = true;
}

} // namespace

CurlWebDavProvider::CurlWebDavProvider() { ensureCurlGlobalInit(); }

CurlWebDavProvider::~CurlWebDavProvider() { disconnect(); }

bool CurlWebDavProvider::connectToHost(const QString &host, int port, const QString &user,
                                       const QString &password, bool useHttps, QString *error) {
    QMutexLocker locker(&m_mutex);
    if (m_connected) {
        if (error)
            *error = QStringLiteral("Already connected");
        return false;
    }
    ensureCurlGlobalInit();

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error)
            *error = QStringLiteral("Failed to initialise curl");
        return false;
    }

    m_errorBuffer[0] = '\0';
    m_host = host;
    m_port = port;
    m_useHttps = useHttps;

    const QString url = davUrl(host, port, useHttps, QStringLiteral("/"), true);
    const QByteArray urlUtf8 = url.toUtf8();
    const QByteArray userUtf8 = user.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, m_errorBuffer);
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // These timeouts persist on the handle (kept as m_curl) and bound every
    // later control-plane request (PROPFIND/MOVE/DELETE/MKCOL/size). m_curl
    // never drives a bulk transfer (those use per-handle curl handles), so a
    // total CURLOPT_TIMEOUT_MS is safe and guards against a hung HTTP request.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(m_timeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(m_timeoutMs));
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (!user.isEmpty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, userUtf8.constData());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, passUtf8.constData());
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Depth: 0");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
    static const char *body =
        "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop>"
        "<D:resourcetype/></D:prop></D:propfind>";
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(std::strlen(body)));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);

    const bool ok = (res == CURLE_OK) &&
                    (httpCode == 207 || httpCode == 200 || httpCode == 301 || httpCode == 302);
    if (!ok) {
        if (error) {
            if (res != CURLE_OK)
                *error = m_errorBuffer[0] != '\0' ? QString::fromUtf8(m_errorBuffer)
                                                  : QString::fromUtf8(curl_easy_strerror(res));
            else
                *error = QStringLiteral("Server returned HTTP %1").arg(httpCode);
        }
        curl_easy_cleanup(curl);
        m_host.clear();
        return false;
    }

    m_curl = curl;
    m_user = user;
    m_password = password;
    m_connected = true;
    return true;
}

bool CurlWebDavProvider::reconnect(QString *error) {
    // Snapshot credentials before disconnect() clears them. connectToHost() and
    // disconnect() each take m_mutex, so reconnect() must not hold the
    // (non-recursive) lock while calling them.
    QString host, user, password;
    int port;
    bool useHttps;
    {
        QMutexLocker locker(&m_mutex);
        host = m_host;
        port = m_port;
        user = m_user;
        password = m_password;
        useHttps = m_useHttps;
    }
    disconnect();
    return connectToHost(host, port, user, password, useHttps, error);
}

void CurlWebDavProvider::disconnect() {
    QMutexLocker locker(&m_mutex);
    if (m_curl) {
        curl_easy_cleanup(static_cast<CURL *>(m_curl));
        m_curl = nullptr;
    }
    m_connected = false;
    m_host.clear();
    m_user.clear();
    m_password.clear();
}

bool CurlWebDavProvider::isConnected() const {
    QMutexLocker locker(&m_mutex);
    return m_connected;
}

QString CurlWebDavProvider::host() const {
    QMutexLocker locker(&m_mutex);
    return m_host;
}

QString CurlWebDavProvider::displayName() const {
    QMutexLocker locker(&m_mutex);
    if (m_host.isEmpty())
        return {};
    return m_user.isEmpty() ? m_host : m_user + QLatin1Char('@') + m_host;
}

QString CurlWebDavProvider::cleanPath(const QString &path) const {
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList out;
    for (const QString &part : parts) {
        if (part == QStringLiteral("."))
            continue;
        if (part == QStringLiteral("..")) {
            if (!out.isEmpty())
                out.removeLast();
            continue;
        }
        out << part;
    }
    return QLatin1Char('/') + out.join(QLatin1Char('/'));
}

QString CurlWebDavProvider::parentPath(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return QString();
    const int idx = clean.lastIndexOf(QLatin1Char('/'));
    if (idx <= 0)
        return QStringLiteral("/");
    return clean.left(idx);
}

QString CurlWebDavProvider::buildUrl(const QString &path, bool isDirectory) const {
    return davUrl(m_host, m_port, m_useHttps, cleanPath(path), isDirectory);
}

bool CurlWebDavProvider::statEntryLocked(const QString &path, bool *isDirOut) const {
    CURL *curl = static_cast<CURL *>(m_curl);
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Depth: 0");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
    static const char *body =
        "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop>"
        "<D:resourcetype/></D:prop></D:propfind>";

    QByteArray responseBuf;
    const QString url = buildUrl(path, false);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(std::strlen(body)));
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuf);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || (httpCode != 207 && httpCode != 200))
        return false;

    // Pass a basePath that cannot match any real href so the single "self"
    // entry (the only <response> in a Depth:0 reply) is kept.
    const QVector<FileInfo> entries = parsePropfindXml(responseBuf, QString(), true);
    if (entries.isEmpty())
        return false;
    if (isDirOut)
        *isDirOut = entries.first().isDir();
    return true;
}

bool CurlWebDavProvider::isDir(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return true;
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    bool isDirFlag = false;
    if (!statEntryLocked(clean, &isDirFlag))
        return false;
    return isDirFlag;
}

bool CurlWebDavProvider::exists(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return true;
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    return statEntryLocked(clean, nullptr);
}

qint64 CurlWebDavProvider::remoteFileSizeLocked(const QString &path) const {
    CURL *curl = static_cast<CURL *>(m_curl);
    const QString url = buildUrl(path, false);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300)
        return -1;
    curl_off_t sizeOut = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &sizeOut);
    return sizeOut >= 0 ? static_cast<qint64>(sizeOut) : -1;
}

FileProvider::RenameResult CurlWebDavProvider::rename(const QString &path, const QString &newName,
                                                       QString *newPath) {
    const QString oldPath = cleanPath(path);
    const QString parent = parentPath(oldPath);
    const QString parentDir = parent.isEmpty() ? QStringLiteral("/") : parent;
    const QString destPath = cleanPath(parentDir + QLatin1Char('/') + newName);

    if (exists(destPath))
        return RenameResult::AlreadyExists;

    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return RenameResult::Failed;
    CURL *curl = static_cast<CURL *>(m_curl);

    const QString destUrl = buildUrl(destPath, false);
    const QByteArray destHeader = (QStringLiteral("Destination: ") + destUrl).toUtf8();
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, destHeader.constData());
    headers = curl_slist_append(headers, "Overwrite: F");

    const QString srcUrl = buildUrl(oldPath, false);
    const QByteArray srcUtf8 = srcUrl.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, srcUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MOVE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);

    if (res != CURLE_OK)
        return RenameResult::Failed;
    if (httpCode == 412)
        return RenameResult::AlreadyExists;
    if (httpCode < 200 || httpCode >= 300)
        return RenameResult::Failed;

    if (newPath)
        *newPath = destPath;
    return RenameResult::Ok;
}

bool CurlWebDavProvider::remove(const QString &path) {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    CURL *curl = static_cast<CURL *>(m_curl);
    const QString url = buildUrl(clean, false);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    return res == CURLE_OK && httpCode >= 200 && httpCode < 300;
}

bool CurlWebDavProvider::mkdir(const QString &path) {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    CURL *curl = static_cast<CURL *>(m_curl);
    const QString url = buildUrl(clean, true);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MKCOL");
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    // 201 Created, or 405 Method Not Allowed if it already exists — treat the
    // latter as success since FileOperations calls mkdir() unconditionally
    // while recursing into directories that may already exist on the dest.
    return res == CURLE_OK && (httpCode == 201 || httpCode == 405);
}

QVector<FileInfo> CurlWebDavProvider::list(const QString &path, bool showHidden) const {
    const QString dirPath = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return {};
    CURL *curl = static_cast<CURL *>(m_curl);

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
    static const char *body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
        "<D:displayname/><D:resourcetype/><D:getcontentlength/><D:getlastmodified/>"
        "</D:prop></D:propfind>";

    QByteArray responseBuf;
    const QString url = buildUrl(dirPath, true);
    const QByteArray urlUtf8 = url.toUtf8();

    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(std::strlen(body)));
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuf);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || (httpCode != 207 && httpCode != 200))
        return {};

    return parsePropfindXml(responseBuf, dirPath, showHidden);
}

QVector<FileInfo> CurlWebDavProvider::parsePropfindXml(const QByteArray &data,
                                                        const QString &basePath, bool showHidden) {
    QVector<FileInfo> result;
    QXmlStreamReader xml(data);

    struct Entry {
        QString href;
        QString displayName;
        qint64 size = 0;
        QDateTime modified;
        bool isCollection = false;
    };
    Entry current;
    bool inResponse = false;
    bool inResourceType = false;
    QString currentTag;
    QString textBuf;

    const QString cleanBase =
        basePath.endsWith(QLatin1Char('/')) ? basePath.left(basePath.size() - 1) : basePath;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString().toLower();
            if (name == QStringLiteral("response")) {
                inResponse = true;
                current = Entry();
            } else if (inResponse && name == QStringLiteral("resourcetype")) {
                inResourceType = true;
            } else if (inResourceType && name == QStringLiteral("collection")) {
                current.isCollection = true;
            } else if (inResponse &&
                       (name == QStringLiteral("href") || name == QStringLiteral("displayname") ||
                        name == QStringLiteral("getcontentlength") ||
                        name == QStringLiteral("getlastmodified"))) {
                currentTag = name;
                textBuf.clear();
            }
        } else if (xml.isCharacters() && !currentTag.isEmpty()) {
            textBuf += xml.text();
        } else if (xml.isEndElement()) {
            const QString name = xml.name().toString().toLower();
            if (name == QStringLiteral("resourcetype")) {
                inResourceType = false;
            } else if (name == currentTag) {
                if (currentTag == QStringLiteral("href"))
                    current.href = textBuf.trimmed();
                else if (currentTag == QStringLiteral("displayname"))
                    current.displayName = textBuf.trimmed();
                else if (currentTag == QStringLiteral("getcontentlength"))
                    current.size = textBuf.trimmed().toLongLong();
                else if (currentTag == QStringLiteral("getlastmodified")) {
                    static const QLocale enLocale(QLocale::English, QLocale::UnitedStates);
                    current.modified = enLocale.toDateTime(
                        textBuf.trimmed(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
                }
                currentTag.clear();
            } else if (name == QStringLiteral("response")) {
                inResponse = false;
                if (current.href.isEmpty())
                    continue;

                QString path = QUrl::fromPercentEncoding(current.href.toUtf8());
                if (path.contains(QStringLiteral("://"))) {
                    const QUrl asUrl(path);
                    if (asUrl.isValid() && !asUrl.path().isEmpty())
                        path = asUrl.path();
                }
                if (path.size() > 1 && path.endsWith(QLatin1Char('/')))
                    path.chop(1);

                if (path == cleanBase)
                    continue; // the collection's own entry (Depth:1 self-reference)

                QString name2 = current.displayName;
                if (name2.isEmpty()) {
                    const int slash = path.lastIndexOf(QLatin1Char('/'));
                    name2 = slash < 0 ? path : path.mid(slash + 1);
                }
                if (name2.isEmpty())
                    continue;
                if (!showHidden && name2.startsWith(QLatin1Char('.')))
                    continue;

                result.append(FileInfo::fromFields(path, name2, current.size, current.modified,
                                                    current.isCollection, QFile::Permissions()));
            }
        }
    }
    return result;
}

FileHandle *CurlWebDavProvider::openRead(const QString &path) {
    QString h, u, p;
    int port, timeout;
    bool https;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected)
            return nullptr;
        h = m_host;
        port = m_port;
        u = m_user;
        p = m_password;
        https = m_useHttps;
        timeout = m_timeoutMs;
    }
    auto *handle = new WebDavHandle();
    handle->mode = WebDavHandle::Mode::Read;
    handle->path = cleanPath(path);
    handle->host = h;
    handle->port = port;
    handle->user = u;
    handle->password = p;
    handle->useHttps = https;
    handle->timeoutMs = timeout;
    return handle;
}

FileHandle *CurlWebDavProvider::openWrite(const QString &path, bool /*truncate*/) {
    QString h, u, p;
    int port, timeout;
    bool https;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected)
            return nullptr;
        h = m_host;
        port = m_port;
        u = m_user;
        p = m_password;
        https = m_useHttps;
        timeout = m_timeoutMs;
    }
    auto *handle = new WebDavHandle();
    handle->mode = WebDavHandle::Mode::Write;
    handle->path = cleanPath(path);
    handle->host = h;
    handle->port = port;
    handle->user = u;
    handle->password = p;
    handle->useHttps = https;
    handle->timeoutMs = timeout;
    // truncate is intentionally ignored: WebDAV PUT always replaces the whole
    // resource (there is no append mode), and seek() refuses resume for
    // write handles, so every openWrite() ends up doing a full PUT anyway.
    return handle;
}

qint64 CurlWebDavProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    auto *h = static_cast<WebDavHandle *>(handle);
    if (!h || h->mode != WebDavHandle::Mode::Read || maxSize <= 0)
        return -1;
    if (!h->started)
        startWebDavTransfer(h);

    auto &state = *h->state;
    QMutexLocker locker(&state.mutex);
    while (state.buffer.isEmpty() && !state.curlFinished)
        state.cond.wait(&state.mutex);

    if (!state.buffer.isEmpty()) {
        const int n = static_cast<int>(qMin<qint64>(maxSize, state.buffer.size()));
        std::memcpy(buffer, state.buffer.constData(), static_cast<size_t>(n));
        state.buffer.remove(0, n);
        state.cond.wakeAll();
        return n;
    }
    return state.curlOk ? 0 : -1;
}

qint64 CurlWebDavProvider::write(FileHandle *handle, const char *buffer, qint64 size) {
    auto *h = static_cast<WebDavHandle *>(handle);
    if (!h || h->mode != WebDavHandle::Mode::Write || size < 0)
        return -1;
    if (!h->started)
        startWebDavTransfer(h);
    if (size == 0)
        return 0;

    auto &state = *h->state;
    QMutexLocker locker(&state.mutex);
    if (state.curlFinished)
        return -1;
    while (state.buffer.size() >= kPipeCapacity && !state.curlFinished)
        state.cond.wait(&state.mutex);
    if (state.curlFinished)
        return -1;
    state.buffer.append(buffer, static_cast<int>(size));
    state.cond.wakeAll();
    return size;
}

bool CurlWebDavProvider::seek(FileHandle *handle, qint64 offset) {
    auto *h = static_cast<WebDavHandle *>(handle);
    if (!h || h->started)
        return false;
    if (h->mode == WebDavHandle::Mode::Read) {
        h->resumeOffset = offset;
        return true;
    }
    // Write (upload): WebDAV PUT has no standardised append/resume; refuse a
    // nonzero offset so the caller surfaces a clean, retryable error instead
    // of silently uploading only the tail of the file (see class comment).
    return offset == 0;
}

qint64 CurlWebDavProvider::handleSize(FileHandle *handle) {
    auto *h = static_cast<WebDavHandle *>(handle);
    if (!h)
        return -1;
    if (h->cachedSize >= 0)
        return h->cachedSize;
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return -1;
    const qint64 size = remoteFileSizeLocked(h->path);
    h->cachedSize = size;
    return size;
}

void CurlWebDavProvider::closeHandle(FileHandle *handle) {
    closeHandleStatus(handle);
}

bool CurlWebDavProvider::closeHandleStatus(FileHandle *handle) {
    auto *h = static_cast<WebDavHandle *>(handle);
    if (!h)
        return true;
    bool ok = true;
    if (h->started) {
        auto &state = *h->state;
        {
            QMutexLocker locker(&state.mutex);
            if (h->mode == WebDavHandle::Mode::Write)
                state.noMoreInput = true;
            else
                state.aborted = true;
            state.cond.wakeAll();
        }
        if (h->worker.joinable())
            h->worker.join();
        // Only an upload's completion matters for correctness: the PUT result is
        // only known now, after the transfer thread has finished.
        if (h->mode == WebDavHandle::Mode::Write) {
            ok = state.curlOk;
            if (!ok)
                qWarning("CurlWebDavProvider: upload of %s did not complete successfully",
                         qPrintable(h->path));
        }
    }
    delete h;
    return ok;
}
