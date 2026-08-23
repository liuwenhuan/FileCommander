#include "CloudClipboardController.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QMimeData>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>

#include "account/CloudClipboardImageCache.h"
#include "account/DeviceAgent.h"
#include "config/Settings.h"

namespace {
constexpr int kDebounceMs = 300;
constexpr int kMaximumTextBytes = 64 * 1024;
constexpr qint64 kMaximumImageBytes = 25LL * 1024 * 1024;
constexpr qint64 kMaximumPixels = 40LL * 1000 * 1000;
constexpr int kThumbnailEdge = 480;
constexpr int kMaximumThumbnailBytes = 128 * 1024;
constexpr int kMaximumItems = 20;

QDateTime itemDate(const CloudClipboardItem &item) {
    return QDateTime::fromString(item.created, Qt::ISODate).toUTC();
}
} // namespace

CloudClipboardController::CloudClipboardController(Settings &settings, AccountClient *client,
                                                   DeviceAgent *agent, QObject *parent)
    : QObject(parent), m_settings(settings), m_client(client),
      m_clipboard(qApp ? qApp->clipboard() : nullptr) {
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &CloudClipboardController::publishPendingClipboard);
    if (m_clipboard) {
        m_observedDigest = currentClipboardDigest();
        connect(m_clipboard, &QClipboard::dataChanged, this,
                &CloudClipboardController::onClipboardChanged);
    }
    if (!m_client)
        return;
    setAgent(agent);
    connect(m_client, &AccountClient::clipboardReady, this, &CloudClipboardController::acceptUpdate);
    connect(m_client, &AccountClient::clipboardPublished, this,
            [this](const CloudClipboardItem &item) {
                if (item.type == QLatin1String("image")) {
                    auto it = m_pendingOriginals.find(item.sha256.toLower());
                    if (it != m_pendingOriginals.end()) {
                        CloudClipboardImageCache().store(item.id, it.value().first, it.value().second,
                                                          QDateTime::fromString(item.expires, Qt::ISODate));
                        m_pendingOriginals.erase(it);
                        emit localImagePublished();
                    }
                }
                acceptItem(item);
            });
    connect(m_client, &AccountClient::clipboardThumbnailReady, this,
            [this](const QString &id, const QByteArray &thumbnail, const QString &mime) {
                for (CloudClipboardItem &item : m_items) {
                    if (item.id == id) {
                        item.thumbnail = thumbnail;
                        item.thumbnailMime = mime;
                        emit changed();
                        return;
                    }
                }
            });
    connect(m_client, &AccountClient::clipboardItemDeleted, this,
            [this](const QString &id, qint64) {
                if (m_pendingDeleteId == id)
                    m_pendingDeleteId.clear();
                for (int i = m_items.size() - 1; i >= 0; --i) {
                    if (m_items.at(i).id == id)
                        m_items.removeAt(i);
                }
                setState(m_items.isEmpty() ? State::Empty : State::Ready);
            });
    connect(m_client, &AccountClient::clipboardCleared, this, [this](qint64) {
        m_items.clear();
        setState(State::Empty);
    });
    connect(m_client, &AccountClient::loggedOut, this, [this] {
        m_items.clear();
        m_pendingOriginals.clear();
        m_pendingDigest.clear();
        setState(State::SignedOut);
    });
    connect(m_client, &AccountClient::loggedIn, this, [this] { refresh(); });
    connect(m_client, &AccountClient::requestFailed, this, [this](const QString &error) {
        if (!m_pendingDownloadId.isEmpty()) {
            const QString id = m_pendingDownloadId;
            m_pendingDownloadId.clear();
            emit imageDownloadFailed(id, error);
        } else if (!m_pendingDeleteId.isEmpty()) {
            m_pendingDeleteId.clear();
            refresh();
        } else if (m_state == State::Loading) {
            setState(State::Error, error);
        }
    });
    connect(m_client, &AccountClient::clipboardSessionReady, this, [this](const AccountSession &session) {
        if (!session.clipboardItemId.isEmpty())
            emit imageSessionReady(session);
    });
}

CloudClipboardController::State CloudClipboardController::state() const { return m_state; }
QString CloudClipboardController::error() const { return m_error; }
const QVector<CloudClipboardItem> &CloudClipboardController::items() const { return m_items; }
bool CloudClipboardController::autoUpload() const { return m_settings.cloudClipboardAutoUpload(); }
bool CloudClipboardController::autoReceive() const { return m_settings.cloudClipboardAutoReceive(); }

void CloudClipboardController::setAgent(DeviceAgent *agent) {
    if (m_agentConnection)
        disconnect(m_agentConnection);
    if (!agent) {
        m_agentConnection = {};
        return;
    }
    m_agentConnection = connect(agent, &DeviceAgent::clipboardChanged, this,
                                [this](qint64 revision, const QString &) {
        if (m_client && m_client->isLoggedIn()) {
            m_refreshDigest = currentClipboardDigest();
            setState(State::Loading);
            m_client->fetchClipboard(revision > 0 ? revision - 1 : 0);
        }
    });
}

void CloudClipboardController::setDevices(const QVector<AccountDeviceInfo> &devices) {
    m_deviceNames.clear();
    for (const AccountDeviceInfo &device : devices)
        m_deviceNames.insert(device.id, device.self ? tr("This device")
                                                    : (device.name.isEmpty() ? tr("Other device")
                                                                             : device.name));
    emit changed();
}

QString CloudClipboardController::deviceName(const QString &deviceId) const {
    if (deviceId == (m_client ? m_client->account().deviceId : QString()))
        return tr("This device");
    return m_deviceNames.value(deviceId, tr("Other device"));
}

void CloudClipboardController::setState(State state, const QString &error) {
    if (m_state == state && m_error == error)
        return;
    m_state = state;
    m_error = error;
    emit changed();
}

void CloudClipboardController::refresh() {
    if (!m_client || !m_client->isLoggedIn()) {
        setState(State::SignedOut);
        return;
    }
    m_refreshDigest = currentClipboardDigest();
    setState(State::Loading);
    m_client->fetchClipboard();
}

void CloudClipboardController::sendText(const QString &text) {
    if (!m_client || !m_client->isLoggedIn() || text.toUtf8().size() > kMaximumTextBytes)
        return;
    m_client->publishClipboardText(text);
}

bool CloudClipboardController::publishImage(const QImage &image) {
    if (!m_client || !m_client->isLoggedIn())
        return false;
    const QByteArray original = encodePng(image);
    const QByteArray preview = thumbnail(image);
    if (original.isEmpty() || preview.isEmpty())
        return false;
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
    m_pendingOriginals.insert(hash, {original, QStringLiteral("image/png")});
    m_client->publishClipboardImage(preview, QStringLiteral("image/jpeg"),
                                    QStringLiteral("image/png"), original.size(),
                                    image.width(), image.height(), hash);
    return true;
}

bool CloudClipboardController::sendImageFromMimeData(const QMimeData *mime) {
    QImage image;
    if (!acceptsImage(mime, &image))
        return false;
    emit localImagePreview(image);
    const QByteArray imageDigest = digest(encodePng(image), 'i');
    if (imageDigest == m_pendingDigest) {
        m_debounce->stop();
        m_pendingDigest.clear();
    }
    return publishImage(image);
}

void CloudClipboardController::sendCurrentClipboard() {
    if (!m_client || !m_client->isLoggedIn() || !m_clipboard)
        return;
    const QMimeData *mime = m_clipboard->mimeData();
    if (sendImageFromMimeData(mime))
        return;
    QString text;
    if (acceptsText(mime, &text))
        m_client->publishClipboardText(text);
}

void CloudClipboardController::deleteItem(const QString &itemId) {
    if (!m_client || itemId.isEmpty())
        return;
    for (int i = m_items.size() - 1; i >= 0; --i)
        if (m_items.at(i).id == itemId)
            m_items.removeAt(i);
    setState(m_items.isEmpty() ? State::Empty : State::Ready);
    m_pendingDeleteId = itemId;
    m_client->deleteClipboardItem(itemId);
}

void CloudClipboardController::clear() {
    if (m_client)
        m_client->clearClipboard();
}

void CloudClipboardController::requestThumbnail(const QString &itemId) {
    if (m_client && !itemId.isEmpty())
        m_client->fetchClipboardThumbnail(itemId);
}

void CloudClipboardController::requestOriginal(const CloudClipboardItem &item) {
    if (item.type != QLatin1String("image") || item.id.isEmpty())
        return;
    CloudClipboardImageCache cache;
    CloudClipboardImageCache::Entry entry;
    if (cache.lookup(item.id, &entry) && entry.size == item.size &&
        QString::fromLatin1(entry.sha256.toHex()).compare(item.sha256, Qt::CaseInsensitive) == 0) {
        QFile file(entry.filePath);
        QImage image;
        if (file.open(QIODevice::ReadOnly) && image.loadFromData(file.readAll())) {
            m_applyingRemote = true;
            m_clipboard->setImage(image);
            m_applyingRemote = false;
            emit imageCopied(item.id);
            return;
        }
    }
    if (m_client && !item.sourceDeviceId.isEmpty()) {
        m_pendingDownloadId = item.id;
        m_client->openClipboardImageSession(item.sourceDeviceId, item.id);
    }
}

CloudClipboardItem CloudClipboardController::item(const QString &itemId) const {
    for (const CloudClipboardItem &known : m_items)
        if (known.id == itemId)
            return known;
    return {};
}

void CloudClipboardController::reportImageDownloadProgress(const QString &itemId,
                                                           qint64 received, qint64 total) {
    emit imageDownloadProgress(itemId, received, total);
}

void CloudClipboardController::completeImageDownload(const QString &itemId,
                                                     const QByteArray &original) {
    if (m_pendingDownloadId == itemId)
        m_pendingDownloadId.clear();
    const CloudClipboardItem found = item(itemId);
    if (found.id.isEmpty() || original.size() != found.size ||
        QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex())
            .compare(found.sha256, Qt::CaseInsensitive) != 0) {
        failImageDownload(itemId, tr("The downloaded image failed verification."));
        return;
    }
    QImage image;
    if (!image.loadFromData(original)) {
        failImageDownload(itemId, tr("The downloaded image could not be decoded."));
        return;
    }
    CloudClipboardImageCache cache;
    if (!cache.store(itemId, original, found.mime,
                     QDateTime::fromString(found.expires, Qt::ISODate))) {
        failImageDownload(itemId, tr("The downloaded image could not be cached."));
        return;
    }
    m_applyingRemote = true;
    m_clipboard->setImage(image);
    m_applyingRemote = false;
    emit imageCopied(itemId);
}

void CloudClipboardController::failImageDownload(const QString &itemId, const QString &error) {
    if (m_pendingDownloadId == itemId)
        m_pendingDownloadId.clear();
    emit imageDownloadFailed(itemId, error);
}

void CloudClipboardController::setAutoUpload(bool enabled) {
    if (enabled && !m_settings.cloudClipboardPrivacyAcknowledged()) {
        emit privacyWarningRequired();
        return;
    }
    m_settings.setCloudClipboardAutoUpload(enabled);
    if (!enabled)
        m_settings.setCloudClipboardAutoReceive(false);
    emit changed();
}

void CloudClipboardController::confirmAutoUpload(bool accepted) {
    if (!accepted)
        return;
    m_settings.setCloudClipboardPrivacyAcknowledged(true);
    m_settings.setCloudClipboardAutoUpload(true);
    emit changed();
}

void CloudClipboardController::setAutoReceive(bool enabled) {
    m_settings.setCloudClipboardAutoReceive(enabled && autoUpload());
    emit changed();
}

bool CloudClipboardController::isPrivateOrFileMime(const QMimeData *mime) {
    if (!mime || mime->hasUrls())
        return true;
    for (const QString &format : mime->formats()) {
        const QString lower = format.toLower();
        if (lower == QLatin1String("text/uri-list") || lower.contains(QLatin1String("file")) ||
            lower.contains(QLatin1String("password")) || lower.contains(QLatin1String("secret")) ||
            lower.contains(QLatin1String("private")) || lower.contains(QLatin1String("gnome-copied")) ||
            lower.contains(QLatin1String("kde-cutselection"))) {
            return true;
        }
    }
    return false;
}

bool CloudClipboardController::acceptsText(const QMimeData *mime, QString *text) {
    if (!mime || isPrivateOrFileMime(mime) || !mime->hasText())
        return false;
    const QString candidate = mime->text();
    if (candidate.isEmpty() || candidate.toUtf8().size() > kMaximumTextBytes)
        return false;
    if (text)
        *text = candidate;
    return true;
}

bool CloudClipboardController::acceptsImage(const QMimeData *mime, QImage *image) {
    if (!mime)
        return false;
    for (const QString &format : mime->formats()) {
        const QString lower = format.toLower();
        if (lower.contains(QLatin1String("password")) ||
            lower.contains(QLatin1String("secret")) ||
            lower.contains(QLatin1String("private")) ||
            lower.contains(QLatin1String("gnome-copied")) ||
            lower.contains(QLatin1String("kde-cutselection")) ||
            lower.startsWith(QLatin1String("application/x-filecommander")))
            return false;
    }
    for (const QUrl &url : mime->urls())
        if (url.isLocalFile())
            return false;

    QImage candidate = qvariant_cast<QImage>(mime->imageData());
    if (candidate.isNull()) {
        for (const QString &format : mime->formats()) {
            if (!format.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
                continue;
            candidate = QImage::fromData(mime->data(format));
            if (!candidate.isNull())
                break;
        }
    }
    if (candidate.isNull() || qint64(candidate.width()) * candidate.height() > kMaximumPixels)
        return false;
    const QByteArray encoded = encodePng(candidate);
    if (encoded.isEmpty() || encoded.size() > kMaximumImageBytes)
        return false;
    if (image)
        *image = candidate;
    return true;
}

QByteArray CloudClipboardController::digest(const QByteArray &bytes, char kind) {
    QByteArray tagged(1, kind);
    tagged += bytes;
    return QCryptographicHash::hash(tagged, QCryptographicHash::Sha256);
}

QByteArray CloudClipboardController::encodePng(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return bytes;
}

QByteArray CloudClipboardController::thumbnail(const QImage &image) {
    const QImage scaled = image.scaled(kThumbnailEdge, kThumbnailEdge, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
    for (int quality = 88; quality >= 25; quality -= 9) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QImageWriter writer(&buffer, "JPEG");
        writer.setQuality(quality);
        if (buffer.open(QIODevice::WriteOnly) && writer.write(scaled) &&
            bytes.size() <= kMaximumThumbnailBytes) {
            return bytes;
        }
    }
    return {};
}

QByteArray CloudClipboardController::currentClipboardDigest() const {
    if (!m_clipboard)
        return {};
    const QMimeData *mime = m_clipboard->mimeData();
    QImage image;
    if (acceptsImage(mime, &image))
        return digest(encodePng(image), 'i');
    QString text;
    if (acceptsText(mime, &text))
        return digest(text.toUtf8(), 't');
    return {};
}

void CloudClipboardController::onClipboardChanged() {
    const QByteArray current = currentClipboardDigest();
    m_observedDigest = current;
    if (m_applyingRemote || !autoUpload() || current.isEmpty() || current == m_pendingDigest)
        return;
    m_pendingDigest = current;
    m_debounce->start();
}

void CloudClipboardController::publishPendingClipboard() {
    if (!m_client || !m_client->isLoggedIn() || !autoUpload() || m_pendingDigest.isEmpty())
        return;
    const QMimeData *mime = m_clipboard ? m_clipboard->mimeData() : nullptr;
    QImage image;
    if (acceptsImage(mime, &image)) {
        const QByteArray original = encodePng(image);
        if (digest(original, 'i') == m_pendingDigest)
            publishImage(image);
        return;
    }
    QString text;
    if (acceptsText(mime, &text) && digest(text.toUtf8(), 't') == m_pendingDigest)
        m_client->publishClipboardText(text);
}

void CloudClipboardController::acceptUpdate(const CloudClipboardUpdate &update) {
    if (update.cleared)
        m_items.clear();
    for (const QString &id : update.deletedIds) {
        for (int i = m_items.size() - 1; i >= 0; --i) {
            if (m_items.at(i).id == id)
                m_items.removeAt(i);
        }
    }
    for (const CloudClipboardItem &item : update.items)
        acceptItem(item);
    if (m_items.isEmpty())
        setState(State::Empty);
    else {
        setState(State::Ready);
        applyLatestRemoteText();
    }
}

void CloudClipboardController::acceptItem(const CloudClipboardItem &item) {
    if (item.id.isEmpty())
        return;
    for (CloudClipboardItem &known : m_items) {
        if (known.id == item.id) {
            const QByteArray thumbnail = known.thumbnail;
            known = item;
            if (known.thumbnail.isEmpty())
                known.thumbnail = thumbnail;
            emit changed();
            return;
        }
    }
    m_items.append(item);
    std::sort(m_items.begin(), m_items.end(), [](const CloudClipboardItem &left,
                                                   const CloudClipboardItem &right) {
        if (left.revision != right.revision)
            return left.revision > right.revision;
        return itemDate(left) > itemDate(right);
    });
    if (m_items.size() > kMaximumItems)
        m_items.resize(kMaximumItems);
    emit changed();
}

void CloudClipboardController::applyLatestRemoteText() {
    if (!m_clipboard || !autoUpload() || !autoReceive() || m_items.isEmpty())
        return;
    const CloudClipboardItem &item = m_items.first();
    if (item.type != QLatin1String("text") || item.text.isEmpty() || !m_client ||
        item.sourceDeviceId == m_client->account().deviceId ||
        currentClipboardDigest() != m_refreshDigest) {
        return;
    }
    m_applyingRemote = true;
    m_clipboard->setText(item.text);
    m_observedDigest = digest(item.text.toUtf8(), 't');
    m_applyingRemote = false;
}
