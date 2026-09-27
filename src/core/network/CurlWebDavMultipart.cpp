#include "CurlWebDavProvider.h"
#include "TransferErrorMapping.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QUrl>

namespace {

constexpr qint64 kLargestExactJsonInteger = 9007199254740991LL;

struct Endpoint {
    QString host;
    int port = 0;
    QString user;
    QString password;
    QString pin;
    int timeoutMs = 12000;
};

QString endpointUrl(const Endpoint &endpoint, const QString &path) {
    QStringList segments;
    for (const QString &segment : path.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        segments.append(QString::fromUtf8(QUrl::toPercentEncoding(segment)));
    QString url = QStringLiteral("https://") + endpoint.host;
    if (endpoint.port > 0 && endpoint.port != 443)
        url += QLatin1Char(':') + QString::number(endpoint.port);
    return url + QLatin1Char('/') + segments.join(QLatin1Char('/'));
}

struct CurlDeleter {
    void operator()(CURL *curl) const { curl_easy_cleanup(curl); }
};

using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;

void configureCurl(CURL *curl, const Endpoint &endpoint, const QByteArray &url) {
    curl_easy_setopt(curl, CURLOPT_URL, url.constData());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
    curl_easy_setopt(curl, CURLOPT_USERNAME, endpoint.user.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, endpoint.password.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, endpoint.pin.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(endpoint.timeoutMs));
}

size_t receiveJson(char *bytes, size_t size, size_t count, void *opaque) {
    auto *body = static_cast<QByteArray *>(opaque);
    const size_t length = size * count;
    if (length > 64 * 1024 || body->size() > 64 * 1024 - static_cast<int>(length))
        return 0;
    body->append(bytes, static_cast<int>(length));
    return length;
}

struct Reply {
    CURLcode curlCode = CURLE_FAILED_INIT;
    long status = 0;
    QByteArray body;
    QString detail;
};

int checkRequestCancelled(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto *checkpoint = static_cast<const std::function<bool()> *>(opaque);
    return *checkpoint && !(*checkpoint)() ? 1 : 0;
}

Reply request(const Endpoint &endpoint, const QString &path, const char *method,
              const QByteArray &body = {}, int timeoutMs = 1500, CURL *persistent = nullptr,
              const std::function<bool()> *checkpoint = nullptr) {
    Reply reply;
    CurlPtr owned(persistent ? nullptr : curl_easy_init());
    CURL *curl = persistent ? persistent : owned.get();
    if (!curl)
        return reply;
    const QByteArray url = endpointUrl(endpoint, path).toUtf8();
    configureCurl(curl, endpoint, url);
    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(qMin(endpoint.timeoutMs, timeoutMs)));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveJson);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);
    if (checkpoint) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkRequestCancelled);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, checkpoint);
    }
    if (std::strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(body.size()));
        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        reply.curlCode = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
        curl_slist_free_all(headers);
    } else {
        reply.curlCode = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
    }
    reply.detail = QString::fromUtf8(errorBuffer);
    return reply;
}

CloseHandleResult failed(CURLcode code, long status, const QString &detail) {
    const TransferErrorMapping::Result mapped = code == CURLE_OK
        ? TransferErrorMapping::webDavHttpError(status)
        : TransferErrorMapping::curlError(code, detail);
    return {false, mapped.error, mapped.detail};
}

CloseHandleResult invalid(const QString &detail) {
    return {false, FileHandle::StreamError::Other, detail};
}

bool parseInteger(const QJsonValue &value, qint64 *out) {
    if (!value.isDouble())
        return false;
    const double number = value.toDouble(-1);
    if (!std::isfinite(number) || number < 0 || number > double(kLargestExactJsonInteger) ||
        std::floor(number) != number)
        return false;
    *out = static_cast<qint64>(number);
    return true;
}

bool validId(const QString &id) {
    if (id.size() != 32)
        return false;
    for (const QChar c : id) {
        if ((c < QLatin1Char('0') || c > QLatin1Char('9')) &&
            (c < QLatin1Char('a') || c > QLatin1Char('f')))
            return false;
    }
    return true;
}

qint64 partStart(qint64 size, int index) {
    return (size / 3) * index + qMin<qint64>(index, size % 3);
}

bool parseOffsets(const QJsonObject &json, qint64 total, std::array<qint64, 3> *offsets) {
    const QJsonArray values = json.value(QStringLiteral("offsets")).toArray();
    if (values.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        qint64 value = 0;
        if (!parseInteger(values.at(i), &value) ||
            value > partStart(total, i + 1) - partStart(total, i))
            return false;
        (*offsets)[i] = value;
    }
    return true;
}

struct Part {
    Endpoint endpoint;
    QString source;
    QString path;
    qint64 start = 0;
    qint64 offset = 0;
    qint64 remaining = 0;
    qint64 consumed = 0;
    int index = 0;
    std::atomic<qint64> sent{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> *stop = nullptr;
    const std::function<bool()> *checkpoint = nullptr;
    CURLcode curlCode = CURLE_FAILED_INIT;
    long status = 0;
    QString detail;
};

size_t readPart(char *buffer, size_t size, size_t count, void *opaque) {
    auto *context = static_cast<std::pair<Part *, QFile *> *>(opaque);
    Part &part = *context->first;
    if (part.stop->load() || (part.checkpoint && *part.checkpoint && !(*part.checkpoint)())) {
        part.stop->store(true);
        return CURL_READFUNC_ABORT;
    }
    const qint64 wanted = qMin<qint64>(qint64(size * count), part.remaining - part.consumed);
    if (wanted <= 0)
        return 0;
    const qint64 read = context->second->read(buffer, wanted);
    if (read <= 0) {
        part.stop->store(true);
        return CURL_READFUNC_ABORT;
    }
    part.consumed += read;
    return static_cast<size_t>(read);
}

int progressPart(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t sent) {
    Part &part = *static_cast<Part *>(opaque);
    part.sent.store(qBound<qint64>(0, qint64(sent), part.remaining));
    if (part.stop->load() || (part.checkpoint && *part.checkpoint && !(*part.checkpoint)())) {
        part.stop->store(true);
        return 1;
    }
    return 0;
}

void sendPart(Part *part) {
    QFile file(part->source);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(part->start)) {
        part->detail = QStringLiteral("Cannot read source file");
        part->stop->store(true);
        part->finished.store(true);
        return;
    }
    CurlPtr curl(curl_easy_init());
    if (!curl) {
        part->detail = QStringLiteral("Failed to initialise curl");
        part->stop->store(true);
        part->finished.store(true);
        return;
    }
    const QByteArray url = endpointUrl(part->endpoint, part->path).toUtf8();
    configureCurl(curl.get(), part->endpoint, url);
    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer);
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("X-FileCommander-Part-Offset: " +
        QByteArray::number(part->offset)).constData());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE,
                     static_cast<curl_off_t>(part->remaining));
    std::pair<Part *, QFile *> context{part, &file};
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, readPart);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &context);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progressPart);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, part);
    part->curlCode = curl_easy_perform(curl.get());
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &part->status);
    part->detail = QString::fromUtf8(errorBuffer);
    if (part->curlCode != CURLE_OK || part->status != 204)
        part->stop->store(true);
    part->finished.store(true);
    curl_slist_free_all(headers);
}

} // namespace

CloseHandleResult CurlWebDavProvider::uploadLocalFileParallel(
    const QString &source, const QString &destination,
    const std::function<bool(qint64, qint64)> &onProgress,
    const std::function<bool()> &checkpoint, QString *error) {
    auto fail = [error](CloseHandleResult result) {
        if (error)
            *error = result.detail;
        return result;
    };
    Endpoint endpoint;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected || !m_relayDeviceRoute || !m_useHttps || m_pinnedKey.isEmpty() ||
            !m_serverMultipart)
            return fail(invalid(QStringLiteral("Multipart relay upload is not available")));
        endpoint = {m_host, m_port, m_user, m_password, m_pinnedKey, m_timeoutMs};
    }

    QFileInfo before(source);
    const qint64 size = before.size();
    if (!before.isFile() || size <= 20LL * 1024 * 1024 || size > kLargestExactJsonInteger)
        return fail(invalid(QStringLiteral("Source is not a supported large local file")));
    const QDateTime modified = before.lastModified();
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return fail(invalid(QStringLiteral("Cannot read source file")));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!input.atEnd()) {
        if (checkpoint && !checkpoint())
            return fail(invalid(QStringLiteral("Cancelled")));
        const QByteArray block = input.read(1024 * 1024);
        if (block.isEmpty())
            return fail(invalid(QStringLiteral("Cannot hash source file")));
        hash.addData(block);
    }
    if (input.pos() != size)
        return fail(invalid(QStringLiteral("Source file changed while hashing")));
    input.close();
    const QFileInfo afterHash(source);
    if (afterHash.size() != size || afterHash.lastModified() != modified)
        return fail(invalid(QStringLiteral("Source file changed while hashing")));

    const QJsonObject startBody{{QStringLiteral("path"), cleanPath(destination)},
                                {QStringLiteral("size"), double(size)},
                                {QStringLiteral("sourceId"), QString::fromLatin1(hash.result().toHex())}};
    const Reply start = request(endpoint, QStringLiteral("/.filecommander/multipart"), "POST",
                                QJsonDocument(startBody).toJson(QJsonDocument::Compact),
                                endpoint.timeoutMs);
    if (start.curlCode != CURLE_OK || (start.status != 200 && start.status != 201))
        return fail(failed(start.curlCode, start.status, start.detail));
    const QJsonObject session = QJsonDocument::fromJson(start.body).object();
    const QString id = session.value(QStringLiteral("id")).toString();
    std::array<qint64, 3> offsets{};
    if (!validId(id) || !parseOffsets(session, size, &offsets))
        return fail(invalid(QStringLiteral("Invalid multipart session response")));

    std::array<Part, 3> parts;
    std::atomic<bool> stop{false};
    qint64 initial = 0;
    for (int i = 0; i < 3; ++i) {
        Part &part = parts[i];
        part.endpoint = endpoint;
        part.source = source;
        part.path = QStringLiteral("/.filecommander/multipart/%1/%2").arg(id).arg(i);
        part.start = partStart(size, i) + offsets[i];
        part.offset = offsets[i];
        part.remaining = partStart(size, i + 1) - part.start;
        part.index = i;
        part.stop = &stop;
        part.checkpoint = &checkpoint;
        part.finished.store(part.remaining == 0);
        initial += offsets[i];
    }
    qint64 received = initial;
    qint64 sent = initial;
    if (onProgress && !onProgress(sent, received))
        return fail(invalid(QStringLiteral("Cancelled")));

    std::array<std::thread, 3> workers;
    for (int i = 0; i < 3; ++i) {
        if (parts[i].remaining > 0)
            workers[i] = std::thread(sendPart, &parts[i]);
    }
    const QString statusPath = QStringLiteral("/.filecommander/multipart/") + id;
    CurlPtr statusCurl(curl_easy_init());
    if (!statusCurl) {
        stop.store(true);
        for (std::thread &worker : workers) {
            if (worker.joinable())
                worker.join();
        }
        return fail(invalid(QStringLiteral("Failed to initialise progress connection")));
    }
    while (!stop.load()) {
        bool allFinished = true;
        for (const Part &part : parts)
            allFinished &= part.finished.load();
        if (allFinished)
            break;
        const Reply status = request(endpoint, statusPath, "GET", {}, 1500, statusCurl.get());
        if (status.curlCode == CURLE_OK && status.status == 200) {
            const QJsonObject json = QJsonDocument::fromJson(status.body).object();
            qint64 total = -1;
            std::array<qint64, 3> current{};
            if (parseInteger(json.value(QStringLiteral("total")), &total) && total == size &&
                parseOffsets(json, size, &current)) {
                qint64 confirmed = current[0] + current[1] + current[2];
                received = qMax(received, confirmed);
            }
        }
        qint64 uploaded = initial;
        for (const Part &part : parts)
            uploaded += part.sent.load();
        sent = qMax(qMax(sent, uploaded), received);
        if (onProgress && !onProgress(sent, received)) {
            stop.store(true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (std::thread &worker : workers) {
        if (worker.joinable())
            worker.join();
    }
    if (stop.load()) {
        for (const Part &part : parts) {
            if (part.remaining > 0 && part.curlCode != CURLE_OK &&
                part.curlCode != CURLE_ABORTED_BY_CALLBACK)
                return fail(failed(part.curlCode, part.status, part.detail));
            if (part.remaining > 0 && part.status >= 400)
                return fail(failed(CURLE_OK, part.status, part.detail));
        }
        return fail(invalid(QStringLiteral("Cancelled")));
    }
    for (const Part &part : parts) {
        if (part.remaining > 0 && (part.curlCode != CURLE_OK || part.status != 204))
            return fail(failed(part.curlCode, part.status, part.detail));
    }
    // A 204 acknowledges each PUT, but the progress bar must still be backed
    // by the receiver's persisted offsets before the publish request starts.
    for (int attempt = 0; received < size && attempt < 8; ++attempt) {
        if (checkpoint && !checkpoint())
            return fail(invalid(QStringLiteral("Cancelled")));
        const Reply status = request(endpoint, statusPath, "GET", {}, 1500, statusCurl.get());
        if (status.curlCode == CURLE_OK && status.status == 200) {
            const QJsonObject json = QJsonDocument::fromJson(status.body).object();
            qint64 total = -1;
            std::array<qint64, 3> current{};
            if (parseInteger(json.value(QStringLiteral("total")), &total) && total == size &&
                parseOffsets(json, size, &current)) {
                received = qMax(received, current[0] + current[1] + current[2]);
                if (onProgress && !onProgress(qMax(sent, received), received))
                    return fail(invalid(QStringLiteral("Cancelled")));
            }
        }
        if (received < size)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (received != size)
        return fail(invalid(QStringLiteral("Receiver did not confirm all multipart bytes")));
    if (checkpoint && !checkpoint())
        return fail(invalid(QStringLiteral("Cancelled")));
    const QFileInfo beforeCommit(source);
    if (beforeCommit.size() != size || beforeCommit.lastModified() != modified)
        return fail(invalid(QStringLiteral("Source file changed before commit")));

    // Once commit starts, the receiver may publish atomically even if this
    // connection closes. Wait for its answer rather than aborting the request
    // on a late cancel and reporting an outcome we cannot know.
    const Reply commit = request(endpoint, statusPath + QStringLiteral("/commit"), "POST",
                                 {}, 2 * 60 * 60 * 1000);
    if (commit.curlCode != CURLE_OK ||
        (commit.status != 200 && commit.status != 201 && commit.status != 204)) {
        // A disconnected or cancelled commit can still finish at the peer.
        // Never claim failure of the file itself without checking its state.
        const Reply finalState = request(endpoint, statusPath, "GET", {}, 1500, statusCurl.get());
        const QJsonObject state = QJsonDocument::fromJson(finalState.body).object();
        if (finalState.curlCode != CURLE_OK || finalState.status != 200 ||
            state.value(QStringLiteral("state")).toString() != QStringLiteral("complete")) {
            if (commit.curlCode == CURLE_ABORTED_BY_CALLBACK ||
                commit.curlCode == CURLE_OPERATION_TIMEDOUT || commit.curlCode == CURLE_RECV_ERROR)
                return fail(invalid(QStringLiteral("Commit interrupted; receiver result is uncertain")));
            return fail(failed(commit.curlCode, commit.status, commit.detail));
        }
    }
    if (onProgress)
        onProgress(size, size);
    if (error)
        error->clear();
    return {true, FileHandle::StreamError::None, {}};
}
