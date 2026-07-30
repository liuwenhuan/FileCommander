#include "CurlFtpProvider.h"
#include "TransferErrorMapping.h"

#include <curl/curl.h>

#include <cstring>
#include <thread>

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QUrl>
#include <QWaitCondition>

#include "FileInfo.h"
#include "diagnostics/RuntimeCounters.h"

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

// Bounded producer/consumer state shared between a FtpHandle's public
// read()/write() calls (consumer/producer from the caller's point of view)
// and the background thread running curl_easy_perform() for that handle.
constexpr qint64 kPipeCapacity = 512 * 1024;

struct FtpTransferState {
    QMutex mutex;
    QWaitCondition cond;
    QByteArray buffer;
    bool curlFinished = false; // curl_easy_perform() thread has returned
    bool noMoreInput = false;  // write mode: caller signalled EOF (no more write() calls)
    bool aborted = false;      // caller wants to stop the transfer early
    CURLcode curlCode = CURLE_OK;
    long responseCode = 0;
    char errorBuffer[CURL_ERROR_SIZE] = {};
};

// WRITEFUNCTION for a download: append incoming bytes to the pipe, blocking
// (bounded) until the consumer drains space or the transfer is aborted.
size_t downloadWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *state = static_cast<FtpTransferState *>(userdata);
    const size_t total = size * nmemb;
    QMutexLocker locker(&state->mutex);
    while (state->buffer.size() >= kPipeCapacity && !state->aborted)
        state->cond.wait(&state->mutex);
    if (state->aborted)
        return 0; // any value != total aborts the transfer
    state->buffer.append(ptr, static_cast<int>(total));
    state->cond.wakeAll();
    return total;
}

// READFUNCTION for an upload: pull bytes the caller supplied via write();
// blocks until data is available, EOF is signalled, or aborted.
size_t uploadReadCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *state = static_cast<FtpTransferState *>(userdata);
    const size_t want = size * nmemb;
    QMutexLocker locker(&state->mutex);
    while (state->buffer.isEmpty() && !state->noMoreInput && !state->aborted)
        state->cond.wait(&state->mutex);
    if (state->aborted)
        return CURL_READFUNC_ABORT;
    if (state->buffer.isEmpty() && state->noMoreInput)
        return 0; // EOF
    const int n = static_cast<int>(qMin<qint64>(static_cast<qint64>(want), state->buffer.size()));
    std::memcpy(ptr, state->buffer.constData(), static_cast<size_t>(n));
    state->buffer.remove(0, n);
    state->cond.wakeAll();
    return static_cast<size_t>(n);
}

// XFERINFOFUNCTION: lets closeHandle() abort an in-flight transfer promptly
// instead of leaving curl to keep transferring data nobody will read.
int progressCallback(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *state = static_cast<FtpTransferState *>(userdata);
    QMutexLocker locker(&state->mutex);
    return state->aborted ? 1 : 0;
}

QString ftpUrl(const QString &host, int port, const QString &path, bool isDirectory) {
    QStringList rawSegs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList encSegs;
    encSegs.reserve(rawSegs.size());
    for (const QString &seg : rawSegs)
        encSegs << QString::fromUtf8(QUrl::toPercentEncoding(seg));
    QString url = QStringLiteral("ftp://%1:%2/").arg(host).arg(port);
    url += encSegs.join(QLatin1Char('/'));
    if (isDirectory && !url.endsWith(QLatin1Char('/')))
        url += QLatin1Char('/');
    return url;
}

} // namespace

// Opaque per-handle state for a streamed FTP transfer. Lives entirely in this
// translation unit; the public API only ever sees it through FileHandle*.
struct FtpHandle : public FileHandle {
    enum class Mode { Read, Write };

    Mode mode = Mode::Read;
    QString path;
    QString host;
    int port = 21;
    QString user;
    QString password;
    bool truncate = true;    // write mode: STOR (true) vs APPE/resume (false)
    qint64 resumeOffset = 0; // read mode: CURLOPT_RESUME_FROM_LARGE
    bool started = false;    // lazily launched on the first read()/write()
    qint64 cachedSize = -1;
    int timeoutMs = 12000;   // connect-phase timeout for the transfer

    CURL *curl = nullptr;
    std::thread worker;
    std::shared_ptr<FtpTransferState> state = std::make_shared<FtpTransferState>();

    StreamError streamError() const override {
        QMutexLocker locker(&state->mutex);
        if (!state->curlFinished)
            return StreamError::None;
        if (state->responseCode >= 400)
            return TransferErrorMapping::ftpResponseError(state->responseCode).error;
        if (state->curlCode != CURLE_OK)
            return TransferErrorMapping::curlError(state->curlCode,
                                                   QString::fromUtf8(state->errorBuffer))
                .error;
        return StreamError::None;
    }

    QString streamErrorDetail() const override {
        QMutexLocker locker(&state->mutex);
        if (!state->curlFinished)
            return {};
        if (state->responseCode >= 400)
            return TransferErrorMapping::ftpResponseError(state->responseCode).detail;
        if (state->curlCode != CURLE_OK)
            return TransferErrorMapping::curlError(state->curlCode,
                                                   QString::fromUtf8(state->errorBuffer))
                .detail;
        return {};
    }

    ~FtpHandle() override {
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

// Launches the background thread that drives curl_easy_perform() for a
// lazily-created handle. Must only be called once per handle, on the first
// read()/write() call (after any seek() has already recorded the desired
// offset / append mode).
void startFtpTransfer(FtpHandle *h) {
    h->curl = curl_easy_init();
    auto state = h->state;
    if (!h->curl) {
        QMutexLocker locker(&state->mutex);
        state->curlFinished = true;
        state->curlCode = CURLE_FAILED_INIT;
        h->started = true;
        return;
    }

    const QString url = ftpUrl(h->host, h->port, h->path, false);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(h->curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(h->curl, CURLOPT_ERRORBUFFER, state->errorBuffer);
    curl_easy_setopt(h->curl, CURLOPT_NOSIGNAL, 1L);
    // Bound the connect phase only. A total CURLOPT_TIMEOUT_MS is deliberately
    // NOT set here: a legitimate bulk transfer may run far longer than the
    // connect timeout, and the caller can already abort a stalled transfer via
    // the progress callback / closeHandle().
    curl_easy_setopt(h->curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(h->timeoutMs));
    if (!h->user.isEmpty()) {
        const QByteArray userUtf8 = h->user.toUtf8();
        const QByteArray passUtf8 = h->password.toUtf8();
        curl_easy_setopt(h->curl, CURLOPT_USERNAME, userUtf8.constData());
        curl_easy_setopt(h->curl, CURLOPT_PASSWORD, passUtf8.constData());
    }
    curl_easy_setopt(h->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h->curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(h->curl, CURLOPT_XFERINFODATA, state.get());

    if (h->mode == FtpHandle::Mode::Read) {
        curl_easy_setopt(h->curl, CURLOPT_WRITEFUNCTION, downloadWriteCallback);
        curl_easy_setopt(h->curl, CURLOPT_WRITEDATA, state.get());
        if (h->resumeOffset > 0)
            curl_easy_setopt(h->curl, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(h->resumeOffset));
    } else {
        curl_easy_setopt(h->curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h->curl, CURLOPT_READFUNCTION, uploadReadCallback);
        curl_easy_setopt(h->curl, CURLOPT_READDATA, state.get());
        curl_easy_setopt(h->curl, CURLOPT_APPEND, h->truncate ? 0L : 1L);
        curl_easy_setopt(h->curl, CURLOPT_FTP_CREATE_MISSING_DIRS, 0L);
    }

    CURL *curl = h->curl;
    const bool isRead = (h->mode == FtpHandle::Mode::Read);
    h->worker = std::thread([curl, state, isRead]() {
        fc::RuntimeCounterGuard counter(fc::RuntimeCounter::CurlTransfer);
        const CURLcode res = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        QMutexLocker locker(&state->mutex);
        state->curlFinished = true;
        state->curlCode = res;
        state->responseCode = responseCode;
        if (isRead)
            state->cond.wakeAll(); // wake any read() blocked waiting for more bytes
        else
            state->cond.wakeAll(); // wake close/write waiters
    });
    h->started = true;
}

} // namespace

CurlFtpProvider::CurlFtpProvider() { ensureCurlGlobalInit(); }

CurlFtpProvider::~CurlFtpProvider() { disconnect(); }

bool CurlFtpProvider::connectToHost(const QString &host, int port, const QString &user,
                                    const QString &password, QString *error) {
    QMutexLocker locker(&m_mutex);
    if (m_connected) {
        if (error)
            *error = QStringLiteral("Already connected");
        return false;
    }
    m_lastConnectAuthFailed = false;
    ensureCurlGlobalInit();

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error)
            *error = QStringLiteral("Failed to initialise curl");
        return false;
    }

    m_errorBuffer[0] = '\0';
    const QString rootUrl = QStringLiteral("ftp://%1:%2/").arg(host).arg(port);
    const QByteArray rootUtf8 = rootUrl.toUtf8();
    const QByteArray userUtf8 = user.toUtf8();
    const QByteArray passUtf8 = password.toUtf8();

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, m_errorBuffer);
    curl_easy_setopt(curl, CURLOPT_URL, rootUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // These timeouts persist on the handle (kept as m_curl) and so bound every
    // later control-plane request (list/isDir/rename/...). m_curl never drives a
    // bulk file transfer (those use per-handle curl handles), so a total
    // CURLOPT_TIMEOUT_MS is safe here and guards against a hung control channel.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(m_timeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(m_timeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    if (!user.isEmpty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, userUtf8.constData());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, passUtf8.constData());
    }

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

    m_lastConnectAuthFailed = (res == CURLE_LOGIN_DENIED);
    if (res != CURLE_OK) {
        if (error) {
            *error = m_errorBuffer[0] != '\0' ? QString::fromUtf8(m_errorBuffer)
                                              : QString::fromUtf8(curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
        return false;
    }

    m_curl = curl;
    m_host = host;
    m_port = port;
    m_user = user;
    m_password = password;
    m_connected = true;
    return true;
}

bool CurlFtpProvider::reconnect(QString *error) {
    // Snapshot credentials before disconnect() clears them. connectToHost() and
    // disconnect() each take m_mutex, so reconnect() must not hold the
    // (non-recursive) lock while calling them.
    QString host, user, password;
    int port;
    {
        QMutexLocker locker(&m_mutex);
        host = m_host;
        port = m_port;
        user = m_user;
        password = m_password;
    }
    disconnect();
    return connectToHost(host, port, user, password, error);
}

void CurlFtpProvider::disconnect() {
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

bool CurlFtpProvider::isConnected() const {
    QMutexLocker locker(&m_mutex);
    return m_connected;
}

QString CurlFtpProvider::host() const {
    QMutexLocker locker(&m_mutex);
    return m_host;
}

QString CurlFtpProvider::displayName() const {
    QMutexLocker locker(&m_mutex);
    if (m_host.isEmpty())
        return {};
    return m_user.isEmpty() ? m_host : m_user + QLatin1Char('@') + m_host;
}

RemoteLocation CurlFtpProvider::remoteLocation() const {
    QMutexLocker locker(&m_mutex);
    RemoteLocation loc;
    if (m_host.isEmpty())
        return loc; // never connected -- stays invalid
    loc.scheme = QStringLiteral("ftp");
    loc.host = m_host;
    loc.port = m_port;
    loc.user = m_user;
    loc.password = m_password;
    // An FTP login of "anonymous" with no password is the protocol's guest
    // mode; gvfs expresses it as `gio mount -a` rather than a password answer.
    loc.anonymous = m_user.isEmpty() || m_user.compare(QLatin1String("anonymous"),
                                                       Qt::CaseInsensitive) == 0;
    return loc;
}

QString CurlFtpProvider::cleanPath(const QString &path) const {
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

QString CurlFtpProvider::parentPath(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return QString();
    const int idx = clean.lastIndexOf(QLatin1Char('/'));
    if (idx <= 0)
        return QStringLiteral("/");
    return clean.left(idx);
}

QString CurlFtpProvider::buildUrl(const QString &path, bool isDirectory) const {
    return ftpUrl(m_host, m_port, cleanPath(path), isDirectory);
}

int CurlFtpProvider::runQuoteCommandsLocked(const QStringList &commands) const {
    CURL *curl = static_cast<CURL *>(m_curl);
    QVector<QByteArray> owned;
    owned.reserve(commands.size());
    struct curl_slist *quote = nullptr;
    for (const QString &cmd : commands) {
        owned.append(cmd.toUtf8());
        quote = curl_slist_append(quote, owned.last().constData());
    }

    const QString rootUrl = QStringLiteral("ftp://%1:%2/").arg(m_host).arg(m_port);
    const QByteArray rootUtf8 = rootUrl.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, rootUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_QUOTE, quote);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);

    const CURLcode res = curl_easy_perform(curl);

    curl_easy_setopt(curl, CURLOPT_QUOTE, nullptr);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_slist_free_all(quote);

    return static_cast<int>(res);
}

bool CurlFtpProvider::isDir(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return true;
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    const int res = runQuoteCommandsLocked({QStringLiteral("CWD ") + clean});
    return res == CURLE_OK;
}

qint64 CurlFtpProvider::remoteFileSizeLocked(const QString &path) const {
    CURL *curl = static_cast<CURL *>(m_curl);
    const QString url = buildUrl(path, false);
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_QUOTE, nullptr);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    const CURLcode res = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    if (res != CURLE_OK)
        return -1;
    curl_off_t sizeOut = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &sizeOut);
    return sizeOut >= 0 ? static_cast<qint64>(sizeOut) : -1;
}

bool CurlFtpProvider::exists(const QString &path) const {
    const QString clean = cleanPath(path);
    if (clean == QStringLiteral("/"))
        return true;
    if (isDir(clean))
        return true;
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    return remoteFileSizeLocked(clean) >= 0;
}

FileProvider::RenameResult CurlFtpProvider::rename(const QString &path, const QString &newName,
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
    const int res = runQuoteCommandsLocked(
        {QStringLiteral("RNFR ") + oldPath, QStringLiteral("RNTO ") + destPath});
    if (res != CURLE_OK)
        return RenameResult::Failed;
    if (newPath)
        *newPath = destPath;
    return RenameResult::Ok;
}

FileProvider::RenameResult CurlFtpProvider::moveTo(const QString &srcPath, const QString &dstPath) {
    const QString from = cleanPath(srcPath);
    const QString to = cleanPath(dstPath);
    if (from == to)
        return RenameResult::Failed;

    // This check is not an optimisation and must not be dropped: RNFR/RNTO
    // overwrites an occupied destination *silently* (verified -- the command
    // pair returns success and the target's contents are simply gone). It is
    // the only thing standing between a move and destroyed data, so it runs
    // before the rename, unlocked, exactly as rename() above does.
    if (exists(to))
        return RenameResult::AlreadyExists;

    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return RenameResult::Unsupported;

    if (runQuoteCommandsLocked({QStringLiteral("RNFR ") + from, QStringLiteral("RNTO ") + to}) !=
        CURLE_OK) {
        // Every server-side refusal arrives as the same CURLE_QUOTE_ERROR, so
        // "the server won't do this move" is indistinguishable from a real
        // error without parsing response text. Report Unsupported and let the
        // caller fall back rather than guessing from a string.
        return RenameResult::Unsupported;
    }
    return RenameResult::Ok;
}

bool CurlFtpProvider::remove(const QString &path) {
    const QString clean = cleanPath(path);
    const bool dir = isDir(clean); // locks internally; released before we lock below
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    const QString cmd = (dir ? QStringLiteral("RMD ") : QStringLiteral("DELE ")) + clean;
    return runQuoteCommandsLocked({cmd}) == CURLE_OK;
}

bool CurlFtpProvider::mkdir(const QString &path) {
    const QString clean = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return false;
    return runQuoteCommandsLocked({QStringLiteral("MKD ") + clean}) == CURLE_OK;
}

QVector<FileInfo> CurlFtpProvider::list(const QString &path, bool showHidden) const {
    const QString dirPath = cleanPath(path);
    QMutexLocker locker(&m_mutex);
    if (!m_connected)
        return {};
    CURL *curl = static_cast<CURL *>(m_curl);

    QByteArray buffer;
    const QString url = buildUrl(dirPath, true);
    const QByteArray urlUtf8 = url.toUtf8();

    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MLSD");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L);
    curl_easy_setopt(curl, CURLOPT_QUOTE, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);

    if (res == CURLE_OK && !buffer.isEmpty())
        return parseMlsdListing(buffer, dirPath, showHidden);

    // Fallback for servers without MLSD support: classic Unix-style LIST.
    buffer.clear();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        return {};
    return parseUnixListing(buffer, dirPath, showHidden);
}

QVector<FileInfo> CurlFtpProvider::parseMlsdListing(const QByteArray &data, const QString &dirPath,
                                                    bool showHidden) {
    QVector<FileInfo> result;
    const QString text = QString::fromUtf8(data);
    static const QRegularExpression lineSplit(QStringLiteral("\r\n|\n|\r"));
    const QStringList lines = text.split(lineSplit, Qt::SkipEmptyParts);
    const QString base = dirPath.endsWith(QLatin1Char('/')) ? dirPath : dirPath + QLatin1Char('/');

    for (const QString &line : lines) {
        const int sp = line.indexOf(QLatin1Char(' '));
        if (sp < 0)
            continue;
        const QString factsPart = line.left(sp);
        const QString name = line.mid(sp + 1);
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            continue;

        QString type, sizeStr, modifyStr, unixMode;
        const QStringList facts = factsPart.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString &fact : facts) {
            const int eq = fact.indexOf(QLatin1Char('='));
            if (eq < 0)
                continue;
            const QString key = fact.left(eq).toLower();
            const QString val = fact.mid(eq + 1);
            if (key == QStringLiteral("type"))
                type = val.toLower();
            else if (key == QStringLiteral("size"))
                sizeStr = val;
            else if (key == QStringLiteral("modify"))
                modifyStr = val;
            else if (key == QStringLiteral("unix.mode"))
                unixMode = val;
        }
        if (type == QStringLiteral("cdir") || type == QStringLiteral("pdir"))
            continue;

        const bool isDirEntry = (type == QStringLiteral("dir"));
        const qint64 size = sizeStr.isEmpty() ? 0 : sizeStr.toLongLong();
        QDateTime modified;
        if (modifyStr.size() >= 14) {
            modified = QDateTime::fromString(modifyStr.left(14), QStringLiteral("yyyyMMddHHmmss"));
            modified.setTimeSpec(Qt::UTC);
        }
        QFile::Permissions perms;
        if (!unixMode.isEmpty()) {
            bool ok = false;
            const uint mode = unixMode.toUInt(&ok, 8);
            if (ok) {
                if (mode & 0400) perms |= QFile::ReadOwner;
                if (mode & 0200) perms |= QFile::WriteOwner;
                if (mode & 0100) perms |= QFile::ExeOwner;
                if (mode & 0040) perms |= QFile::ReadGroup;
                if (mode & 0020) perms |= QFile::WriteGroup;
                if (mode & 0010) perms |= QFile::ExeGroup;
                if (mode & 0004) perms |= QFile::ReadOther;
                if (mode & 0002) perms |= QFile::WriteOther;
                if (mode & 0001) perms |= QFile::ExeOther;
            }
        }

        result.append(FileInfo::fromFields(base + name, name, size, modified, isDirEntry, perms));
    }
    return result;
}

QVector<FileInfo> CurlFtpProvider::parseUnixListing(const QByteArray &data, const QString &dirPath,
                                                    bool showHidden) {
    QVector<FileInfo> result;
    const QString text = QString::fromUtf8(data);
    static const QRegularExpression lineSplit(QStringLiteral("\r\n|\n|\r"));
    const QStringList lines = text.split(lineSplit, Qt::SkipEmptyParts);
    const QString base = dirPath.endsWith(QLatin1Char('/')) ? dirPath : dirPath + QLatin1Char('/');

    static const QRegularExpression re(QStringLiteral(
        "^([-dlbcps])[-rwxsStT]{9}\\S*\\s+\\S+\\s+\\S+\\s+\\S+\\s+(\\d+)\\s+"
        "([A-Za-z]{3}\\s+\\d{1,2}\\s+(?:\\d{1,2}:\\d{2}|\\d{4}))\\s+(.+)$"));
    static const QLocale enLocale(QLocale::English, QLocale::UnitedStates);

    for (const QString &line : lines) {
        const QRegularExpressionMatch m = re.match(line);
        if (!m.hasMatch())
            continue;
        const QChar typeChar = m.captured(1).at(0);
        const qint64 size = m.captured(2).toLongLong();
        QString name = m.captured(4);
        const int arrow = name.indexOf(QStringLiteral(" -> "));
        if (arrow >= 0)
            name = name.left(arrow);
        name = name.trimmed();
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            continue;

        const bool isDirEntry = (typeChar == QLatin1Char('d'));
        const QString dateStr = m.captured(3);
        QDateTime modified = enLocale.toDateTime(dateStr, QStringLiteral("MMM d yyyy"));
        if (!modified.isValid()) {
            modified = enLocale.toDateTime(
                QStringLiteral("%1 %2").arg(dateStr).arg(QDate::currentDate().year()),
                QStringLiteral("MMM d HH:mm yyyy"));
        }

        result.append(
            FileInfo::fromFields(base + name, name, size, modified, isDirEntry, QFile::Permissions()));
    }
    return result;
}

FileHandle *CurlFtpProvider::openRead(const QString &path) {
    QString h, u, p;
    int port, timeout;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected)
            return nullptr;
        h = m_host;
        port = m_port;
        u = m_user;
        p = m_password;
        timeout = m_timeoutMs;
    }
    auto *handle = new FtpHandle();
    handle->mode = FtpHandle::Mode::Read;
    handle->path = cleanPath(path);
    handle->host = h;
    handle->port = port;
    handle->user = u;
    handle->password = p;
    handle->timeoutMs = timeout;
    return handle;
}

FileHandle *CurlFtpProvider::openWrite(const QString &path, bool truncate) {
    QString h, u, p;
    int port, timeout;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected)
            return nullptr;
        h = m_host;
        port = m_port;
        u = m_user;
        p = m_password;
        timeout = m_timeoutMs;
    }
    auto *handle = new FtpHandle();
    handle->mode = FtpHandle::Mode::Write;
    handle->path = cleanPath(path);
    handle->host = h;
    handle->port = port;
    handle->user = u;
    handle->password = p;
    handle->truncate = truncate;
    handle->timeoutMs = timeout;
    return handle;
}

qint64 CurlFtpProvider::read(FileHandle *handle, char *buffer, qint64 maxSize) {
    auto *h = static_cast<FtpHandle *>(handle);
    if (!h || h->mode != FtpHandle::Mode::Read || maxSize <= 0)
        return -1;
    if (!h->started)
        startFtpTransfer(h);

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
    return state.curlCode == CURLE_OK ? 0 : -1;
}

qint64 CurlFtpProvider::write(FileHandle *handle, const char *buffer, qint64 size) {
    auto *h = static_cast<FtpHandle *>(handle);
    if (!h || h->mode != FtpHandle::Mode::Write || size < 0)
        return -1;
    if (!h->started)
        startFtpTransfer(h);
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

bool CurlFtpProvider::seek(FileHandle *handle, qint64 offset) {
    auto *h = static_cast<FtpHandle *>(handle);
    if (!h || h->started)
        return false;
    if (h->mode == FtpHandle::Mode::Read)
        h->resumeOffset = offset;
    // Write mode: APPE/STOR mode was already decided by openWrite()'s
    // truncate flag; the offset is informational only.
    return true;
}

qint64 CurlFtpProvider::handleSize(FileHandle *handle) {
    auto *h = static_cast<FtpHandle *>(handle);
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

void CurlFtpProvider::closeHandle(FileHandle *handle) {
    (void)closeHandleResult(handle);
}

bool CurlFtpProvider::closeHandleStatus(FileHandle *handle) {
    return closeHandleResult(handle).committed;
}

CloseHandleResult CurlFtpProvider::closeHandleResult(FileHandle *handle) {
    auto *h = static_cast<FtpHandle *>(handle);
    if (!h)
        return {};

    CloseHandleResult result;
    if (h->started) {
        auto &state = *h->state;
        {
            QMutexLocker locker(&state.mutex);
            if (h->mode == FtpHandle::Mode::Write)
                state.noMoreInput = true;
            else
                state.aborted = true;
            state.cond.wakeAll();
        }
        if (h->worker.joinable())
            h->worker.join();
        if (h->mode == FtpHandle::Mode::Write) {
            result.error = h->streamError();
            result.detail = h->streamErrorDetail();
            result.committed = (result.error == FileHandle::StreamError::None);
        }
    }
    delete h;
    return result;
}
