#include "CloudClipboardController.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QFutureWatcher>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QtConcurrent>

#include "account/DeviceAgent.h"
#include "config/Settings.h"

namespace {

QString noOtherDevicesMessage() {
    return QObject::tr("No other devices online or registered");
}

QDateTime deliveryDate(const ClipboardDeliveryInfo &delivery) {
    QDateTime date = QDateTime::fromString(delivery.created, Qt::ISODateWithMs);
    if (!date.isValid())
        date = QDateTime::fromString(delivery.created, Qt::ISODate);
    return date;
}

} // namespace

CloudClipboardController::CloudClipboardController(Settings &settings, AccountClient *client,
                                                   DeviceAgent *agent, QObject *parent)
    : QObject(parent), m_ownedStore(std::make_unique<ClipboardHistoryStore>()),
      m_store(m_ownedStore.get()) {
    Q_UNUSED(settings);
    initialize(client, agent);
}

CloudClipboardController::CloudClipboardController(ClipboardHistoryStore &store, AccountClient *client,
                                                   DeviceAgent *agent, QObject *parent)
    : QObject(parent), m_store(&store) {
    initialize(client, agent);
}

void CloudClipboardController::initialize(AccountClient *client, DeviceAgent *agent) {
    m_client = client;
    m_clipboard = qApp ? qApp->clipboard() : nullptr;
    m_downloadDirectory = std::make_unique<QTemporaryDir>();
    if (m_clipboard)
        connect(m_clipboard, &QClipboard::dataChanged, this, &CloudClipboardController::onClipboardChanged);

    if (!m_client)
        return;

    connect(m_client, &AccountClient::loggedIn, this, [this](const AccountInfo &) { refresh(); });
    connect(m_client, &AccountClient::loggedOut, this, [this] {
        m_deliveries.clear();
        m_completedDeliveryIds.clear();
        m_activeSendRecordId.clear();
        setState(State::SignedOut);
    });
    connect(m_client, &AccountClient::clipboardSendProgress, this,
            [this](qint64 sent, qint64 total) {
                if (!m_activeSendRecordId.isEmpty())
                    emit transferProgress(m_activeSendRecordId, sent, total);
            });
    connect(m_client, &AccountClient::clipboardSendFinished, this,
            [this](const QString &, int recipientCount) {
                if (m_activeSendRecordId.isEmpty())
                    return;
                const QString recordId = m_activeSendRecordId;
                m_activeSendRecordId.clear();
                if (recipientCount == 0)
                    setTransferStatus(noOtherDevicesMessage());
                else
                    setTransferStatus(tr("Sent to %1 device(s).").arg(recipientCount));
                emit transferFinished(recordId);
            });
    connect(m_client, &AccountClient::clipboardSendFailed, this, [this](const QString &error) {
        if (m_activeSendRecordId.isEmpty())
            return;
        const QString recordId = m_activeSendRecordId;
        m_activeSendRecordId.clear();
        setTransferStatus(error);
        emit transferFinished(recordId);
    });
    connect(m_client, &AccountClient::clipboardDeliveriesReady, this,
            &CloudClipboardController::receiveDeliveries);
    connect(m_client, &AccountClient::clipboardDownloadProgress, this,
            [this](const QString &id, qint64 received, qint64 total) {
                emit transferProgress(id, received, total);
            });
    connect(m_client, &AccountClient::clipboardDownloadFinished, this,
            &CloudClipboardController::finishDeliveryDownload);
    connect(m_client, &AccountClient::clipboardDeliveryDownloadFailed, this,
            [this](const QString &id, const QString &error) {
                m_deliveries.remove(id);
                setTransferStatus(error);
                emit transferFinished(id);
            });
    connect(m_client, &AccountClient::clipboardDeliveryAcknowledged, this,
            [this](const QString &id) {
                if (m_deliveries.remove(id) > 0)
                    m_completedDeliveryIds.insert(id);
            });
    connect(m_client, &AccountClient::clipboardDeliveryAcknowledgementFailed, this,
            [this](const QString &id, const QString &error) {
                auto delivery = m_deliveries.find(id);
                if (delivery == m_deliveries.end())
                    return;
                delivery->state = DeliveryState::AwaitingAcknowledgement;
                setTransferStatus(error);
            });
    connect(m_client, &AccountClient::requestFailed, this, [this](const QString &error) {
        if (m_state == State::Loading)
            setState(State::Error, error);
    });

    setAgent(agent);
    if (m_client->isLoggedIn())
        refresh();
}

CloudClipboardController::State CloudClipboardController::state() const { return m_state; }
QString CloudClipboardController::error() const { return m_error; }
const QVector<ClipboardHistoryRecord> &CloudClipboardController::items() const {
    return m_store->records();
}

ClipboardHistoryRecord CloudClipboardController::record(const QString &id) const {
    ClipboardHistoryRecord found;
    if (m_store)
        m_store->lookup(id, &found);
    return found;
}

QString CloudClipboardController::selectedRecordId() const { return m_selectedRecordId; }

void CloudClipboardController::setAgent(DeviceAgent *agent) {
    if (m_agentConnection)
        disconnect(m_agentConnection);
    if (m_agentReadyConnection)
        disconnect(m_agentReadyConnection);
    m_agentConnection = {};
    m_agentReadyConnection = {};
    if (!agent)
        return;
    m_agentConnection = connect(agent, &DeviceAgent::clipboardDeliveryAvailable, this,
                                [this](const QString &) { refresh(); });
    m_agentReadyConnection = connect(agent, &DeviceAgent::announced, this,
                                     &CloudClipboardController::refresh);
}

void CloudClipboardController::setDevices(const QVector<AccountDeviceInfo> &devices) {
    m_devices = devices;
    m_deviceNames.clear();
    for (const AccountDeviceInfo &device : devices) {
        m_deviceNames.insert(device.id, device.self ? tr("This device")
                                                     : (device.name.isEmpty() ? tr("Other device")
                                                                              : device.name));
    }
    emit changed();
}

QString CloudClipboardController::deviceName(const QString &deviceId) const {
    if (deviceId.isEmpty())
        return tr("This device");
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

void CloudClipboardController::setTransferStatus(const QString &status) {
    if (m_transferStatus == status)
        return;
    m_transferStatus = status;
    emit transferStatusChanged(status);
    emit changed();
}

void CloudClipboardController::refresh() {
    if (!m_client || !m_client->isLoggedIn()) {
        setState(State::SignedOut);
        return;
    }
    setState(State::Loading);
    m_client->fetchClipboardDeliveries();
}

void CloudClipboardController::selectRecord(const QString &recordId) {
    if (recordId == m_selectedRecordId)
        return;
    m_selectedRecordId = m_store->lookup(recordId) ? recordId : QString();
    emit selectionChanged(m_selectedRecordId);
    emit changed();
}

bool CloudClipboardController::copyRecordToClipboard(const QString &recordId) {
    if (!m_clipboard)
        return false;
    const ClipboardHistoryRecord found = record(recordId);
    if (found.id.isEmpty())
        return false;

    QByteArray fingerprint;
    if (found.kind == ClipboardRecordKind::Text) {
        fingerprint = QCryptographicHash::hash(QByteArrayLiteral("t") + found.text.toUtf8(),
                                               QCryptographicHash::Sha256);
        m_ignoredClipboardFingerprint = fingerprint;
        m_copyingToClipboard = true;
        m_clipboard->setText(found.text);
        m_copyingToClipboard = false;
    } else {
        QFile imageFile(found.imagePath);
        QImage image;
        if (!imageFile.open(QIODevice::ReadOnly) || !image.loadFromData(imageFile.readAll()))
            return false;
        fingerprint = QCryptographicHash::hash(QByteArrayLiteral("i") + encodePng(image),
                                               QCryptographicHash::Sha256);
        m_ignoredClipboardFingerprint = fingerprint;
        m_copyingToClipboard = true;
        m_clipboard->setImage(image);
        m_copyingToClipboard = false;
    }
    return true;
}

bool CloudClipboardController::hasOtherDevices() const {
    for (const AccountDeviceInfo &device : m_devices) {
        if (!device.self && !device.id.isEmpty())
            return true;
    }
    return false;
}

void CloudClipboardController::sendRecord(const QString &recordId) {
    const ClipboardHistoryRecord found = record(recordId);
    if (found.id.isEmpty() || found.origin != ClipboardRecordOrigin::Local)
        return;
    if (!m_activeSendRecordId.isEmpty()) {
        setTransferStatus(tr("A clipboard delivery is already being sent."));
        return;
    }
    selectRecord(recordId);
    if (!m_client || !m_client->isLoggedIn()) {
        setState(State::SignedOut);
        return;
    }
    if (!hasOtherDevices()) {
        setTransferStatus(noOtherDevicesMessage());
        return;
    }

    m_activeSendRecordId = recordId;
    setTransferStatus(tr("Sending..."));
    if (found.kind == ClipboardRecordKind::Text) {
        m_client->sendClipboardText(found.text);
        return;
    }
    m_client->sendClipboardImageFile(found.imagePath, found.mime, found.width, found.height,
                                     QByteArray::fromHex(found.sha256.toLatin1()));
}

bool CloudClipboardController::removeRecord(const QString &recordId) {
    if (!m_store->remove(recordId))
        return false;
    if (m_selectedRecordId == recordId) {
        m_selectedRecordId.clear();
        emit selectionChanged(QString());
    }
    emit changed();
    return true;
}

void CloudClipboardController::clear() {
    const QVector<ClipboardHistoryRecord> records = items();
    for (const ClipboardHistoryRecord &known : records)
        m_store->remove(known.id);
    if (!m_selectedRecordId.isEmpty()) {
        m_selectedRecordId.clear();
        emit selectionChanged(QString());
    }
    emit changed();
}

void CloudClipboardController::onClipboardChanged() {
    if (!m_clipboard)
        return;
    const QByteArray fingerprint = clipboardFingerprint(m_clipboard->mimeData());
    if (m_copyingToClipboard) {
        if (fingerprint == m_ignoredClipboardFingerprint)
            m_ignoredClipboardFingerprint.clear();
        return;
    }
    if (!m_ignoredClipboardFingerprint.isEmpty() && fingerprint == m_ignoredClipboardFingerprint) {
        m_ignoredClipboardFingerprint.clear();
        return;
    }
    captureClipboard();
}

void CloudClipboardController::captureClipboard() {
    ClipboardCapture capture;
    if (!m_clipboard || !ClipboardHistoryStore::captureFromMimeData(m_clipboard->mimeData(), &capture))
        return;
    if (capture.kind == ClipboardRecordKind::Text) {
        if (!m_store->addLocalText(capture.text).id.isEmpty())
            emit changed();
        return;
    }
    addCapturedImage(capture.image);
}

void CloudClipboardController::addCapturedImage(const QImage &image) {
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, image] {
        const QByteArray encoded = watcher->result();
        watcher->deleteLater();
        if (!encoded.isEmpty() &&
            !m_store->addLocalImage(encoded, QStringLiteral("image/png"), image.width(), image.height()).id.isEmpty()) {
            emit changed();
        }
    });
    watcher->setFuture(QtConcurrent::run([image] { return encodePng(image); }));
}

void CloudClipboardController::receiveDeliveries(const QVector<ClipboardDeliveryInfo> &deliveries) {
    if (m_client && m_client->isLoggedIn())
        setState(deliveries.isEmpty() && items().isEmpty() ? State::Empty : State::Ready);
    if (!m_downloadDirectory || !m_downloadDirectory->isValid()) {
        setTransferStatus(tr("Could not prepare clipboard download storage."));
        return;
    }
    for (const ClipboardDeliveryInfo &delivery : deliveries) {
        if (delivery.id.isEmpty() || m_completedDeliveryIds.contains(delivery.id))
            continue;
        auto known = m_deliveries.find(delivery.id);
        if (known != m_deliveries.end()) {
            if (known->state == DeliveryState::AwaitingAcknowledgement) {
                known->state = DeliveryState::Acknowledging;
                m_client->acknowledgeClipboardDelivery(delivery.id);
            }
            continue;
        }
        m_deliveries.insert(delivery.id, {delivery, DeliveryState::Downloading});
        setTransferStatus(tr("Receiving..."));
        const QByteArray safeName = QCryptographicHash::hash(delivery.id.toUtf8(),
                                                             QCryptographicHash::Sha256).toHex();
        const QString partPath = m_downloadDirectory->filePath(
            QString::fromLatin1(safeName) + QStringLiteral(".part"));
        m_client->downloadClipboardDelivery(delivery, partPath);
    }
}

void CloudClipboardController::finishDeliveryDownload(const QString &deliveryId, const QString &partPath) {
    auto found = m_deliveries.find(deliveryId);
    if (found == m_deliveries.end() || found->state != DeliveryState::Downloading)
        return;
    const ClipboardDeliveryInfo delivery = found->info;

    ClipboardHistoryRecord stored;
    if (delivery.kind == QLatin1String("text")) {
        QFile part(partPath);
        const QByteArray bytes = part.open(QIODevice::ReadOnly) ? part.readAll() : QByteArray();
        const QByteArray expected = QByteArray::fromHex(delivery.sha256.toLatin1());
        if (bytes.size() == delivery.size && expected.size() == 32 &&
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == expected) {
            stored = m_store->addIncomingText(QString::fromUtf8(bytes), delivery.sourceDeviceId,
                                              delivery.sourceDeviceName, deliveryDate(delivery));
        }
    } else if (delivery.kind == QLatin1String("image")) {
        stored = m_store->addIncomingImageFile(partPath, delivery.mime, delivery.size, delivery.width,
                                               delivery.height, delivery.sha256, delivery.sourceDeviceId,
                                               delivery.sourceDeviceName, deliveryDate(delivery));
    }
    QFile::remove(partPath);

    if (stored.id.isEmpty()) {
        m_deliveries.erase(found);
        setTransferStatus(tr("Could not save the clipboard delivery."));
        emit transferFinished(deliveryId);
        return;
    }
    found->state = DeliveryState::Acknowledging;
    emit changed();
    setTransferStatus(tr("Delivery received."));
    emit transferFinished(deliveryId);
    if (m_client)
        m_client->acknowledgeClipboardDelivery(delivery.id);
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
    if (candidate.isEmpty() || candidate.toUtf8().size() > ClipboardHistoryStore::maximumTextBytes)
        return false;
    if (text)
        *text = candidate;
    return true;
}

bool CloudClipboardController::acceptsImage(const QMimeData *mime, QImage *image) {
    if (!mime || isPrivateOrFileMime(mime))
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
    if (candidate.isNull() || qint64(candidate.width()) * candidate.height() > ClipboardHistoryStore::maximumPixels)
        return false;
    const QByteArray encoded = encodePng(candidate);
    if (encoded.isEmpty() || encoded.size() > ClipboardHistoryStore::maximumImageBytes)
        return false;
    if (image)
        *image = candidate;
    return true;
}

QByteArray CloudClipboardController::encodePng(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return bytes;
}

QByteArray CloudClipboardController::clipboardFingerprint(const QMimeData *mime) {
    QImage image;
    if (acceptsImage(mime, &image))
        return QCryptographicHash::hash(QByteArrayLiteral("i") + encodePng(image),
                                        QCryptographicHash::Sha256);
    QString text;
    if (acceptsText(mime, &text))
        return QCryptographicHash::hash(QByteArrayLiteral("t") + text.toUtf8(),
                                        QCryptographicHash::Sha256);
    return {};
}
