#include "CloudClipboardController.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QFutureWatcher>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>

#include "account/DeviceAgent.h"
#include "config/Settings.h"

namespace {

QString noOtherDevicesMessage() {
    return QObject::tr("No other devices registered");
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
      m_store(m_ownedStore.get()), m_settings(&settings),
      m_autoSendEnabled(settings.cloudClipboardAutoSend()),
      m_selectedTargetDeviceId(settings.cloudClipboardTargetDeviceId()) {
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
        const bool removedIncoming = m_store->removeIncomingRecords();
        if (!m_selectedRecordId.isEmpty() && !m_store->lookup(m_selectedRecordId)) {
            m_selectedRecordId.clear();
            emit selectionChanged(QString());
        }
        ++m_autoSendGeneration;
        m_lastAutomaticFingerprint.clear();
        m_deliveries.clear();
        m_completedDeliveryIds.clear();
        clearSendQueue();
        setTransferStatus(QString());
        emit transferReset();
        if (removedIncoming)
            emit changed();
        setState(State::SignedOut);
    });
    connect(m_client, &AccountClient::clipboardSendProgress, this,
            [this](qint64 sent, qint64 total) {
                if (!m_activeSend.recordId.isEmpty())
                    emit transferProgress(m_activeSend.recordId, sent, total);
            });
    connect(m_client, &AccountClient::clipboardSendFinished, this,
            [this](const QString &, int recipientCount) { finishActiveSend(recipientCount); });
    connect(m_client, &AccountClient::clipboardSendFailed, this,
            [this](const QString &error) { finishActiveSend(-1, error); });
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
    emit devicesChanged();
    emit changed();
}

const QVector<AccountDeviceInfo> &CloudClipboardController::devices() const { return m_devices; }

bool CloudClipboardController::autoSendEnabled() const { return m_autoSendEnabled; }

void CloudClipboardController::setAutoSendEnabled(bool enabled) {
    if (m_autoSendEnabled == enabled)
        return;
    m_autoSendEnabled = enabled;
    ++m_autoSendGeneration;
    m_lastAutomaticFingerprint.clear();
    if (m_settings)
        m_settings->setCloudClipboardAutoSend(enabled);
    if (!enabled) {
        const int before = m_pendingSends.size();
        m_pendingSends.erase(std::remove_if(m_pendingSends.begin(), m_pendingSends.end(),
                                             [](const PendingSend &entry) {
                                                 return entry.origin == SendOrigin::Automatic;
                                             }),
                             m_pendingSends.end());
        const int removed = before - m_pendingSends.size();
        m_sendBatchTotal = qMax(m_sendBatchCompleted +
                                    (m_activeSend.recordId.isEmpty() ? 0 : 1),
                                m_sendBatchTotal - removed);
        if (m_activeSend.recordId.isEmpty() && m_pendingSends.isEmpty())
            finishSendBatch();
    }
    emit autoSendChanged(enabled);
}

QString CloudClipboardController::selectedTargetDeviceId() const {
    return m_selectedTargetDeviceId;
}

void CloudClipboardController::setSelectedTargetDeviceId(const QString &deviceId) {
    if (!deviceId.isEmpty() && !isKnownTarget(deviceId))
        return;
    if (m_selectedTargetDeviceId == deviceId)
        return;
    m_selectedTargetDeviceId = deviceId;
    if (m_settings)
        m_settings->setCloudClipboardTargetDeviceId(deviceId);
    emit selectedTargetDeviceChanged(deviceId);
}

bool CloudClipboardController::targetDeviceIsAvailable() const {
    return m_selectedTargetDeviceId.isEmpty() || isKnownTarget(m_selectedTargetDeviceId);
}

bool CloudClipboardController::isKnownTarget(const QString &deviceId) const {
    for (const AccountDeviceInfo &device : m_devices)
        if (!device.self && device.id == deviceId)
            return true;
    return false;
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
    const QString selected = m_store->lookup(recordId) ? recordId : QString();
    if (selected == m_selectedRecordId)
        return;
    m_selectedRecordId = selected;
    emit selectionChanged(m_selectedRecordId);
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

void CloudClipboardController::sendRecord(const QString &recordId, const QString &targetDeviceId) {
    sendRecords({recordId}, targetDeviceId);
}

void CloudClipboardController::sendRecords(const QStringList &recordIds, const QString &targetDeviceId) {
    enqueueRecords(recordIds, targetDeviceId, SendOrigin::Manual);
}

bool CloudClipboardController::enqueueRecords(const QStringList &recordIds,
                                              const QString &targetDeviceId, SendOrigin origin) {
    if (!m_client || !m_client->isLoggedIn()) {
        if (origin == SendOrigin::Manual)
            setState(State::SignedOut);
        return false;
    }
    if (!targetDeviceId.isEmpty() && !isKnownTarget(targetDeviceId)) {
        setTransferStatus(tr("The selected clipboard device is unavailable."));
        return false;
    }
    QSet<QString> seen;
    int accepted = 0;
    for (const QString &id : recordIds) {
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        const ClipboardHistoryRecord found = record(id);
        if (found.id.isEmpty() || found.origin != ClipboardRecordOrigin::Local)
            continue;
        const auto duplicate = [&id, &targetDeviceId](const PendingSend &entry) {
            return entry.recordId == id && entry.targetDeviceId == targetDeviceId;
        };
        auto pending = std::find_if(m_pendingSends.begin(), m_pendingSends.end(), duplicate);
        if (pending != m_pendingSends.end()) {
            if (origin == SendOrigin::Manual && pending->origin == SendOrigin::Automatic)
                pending->origin = SendOrigin::Manual;
            continue;
        }
        if (duplicate(m_activeSend))
            continue;
        m_pendingSends.append({id, targetDeviceId, origin});
        ++accepted;
    }
    if (!accepted)
        return false;
    if (m_activeSend.recordId.isEmpty() && m_pendingSends.size() == accepted) {
        m_sendBatchTotal = m_sendBatchCompleted = m_sendBatchDelivered = 0;
        m_sendBatchNoRecipients = m_sendBatchFailures = 0;
    }
    m_sendBatchTotal += accepted;
    scheduleNextSend();
    return true;
}

void CloudClipboardController::scheduleNextSend() {
    if (m_nextSendScheduled || !m_activeSend.recordId.isEmpty() || m_pendingSends.isEmpty())
        return;
    m_nextSendScheduled = true;
    QTimer::singleShot(0, this, [this] { m_nextSendScheduled = false; startNextSend(); });
}

void CloudClipboardController::startNextSend() {
    if (!m_activeSend.recordId.isEmpty())
        return;
    while (!m_pendingSends.isEmpty()) {
        const PendingSend entry = m_pendingSends.takeFirst();
        const ClipboardHistoryRecord found = record(entry.recordId);
        if (found.id.isEmpty() || found.origin != ClipboardRecordOrigin::Local ||
            (!entry.targetDeviceId.isEmpty() && !isKnownTarget(entry.targetDeviceId))) {
            --m_sendBatchTotal;
            continue;
        }
        m_activeSend = entry;
        setTransferStatus(entry.origin == SendOrigin::Automatic
                              ? tr("Automatically queueing %1 of %2...").arg(m_sendBatchCompleted + 1).arg(m_sendBatchTotal)
                              : tr("Sending %1 of %2...").arg(m_sendBatchCompleted + 1).arg(m_sendBatchTotal));
        if (found.kind == ClipboardRecordKind::Text) {
            entry.targetDeviceId.isEmpty() ? m_client->sendClipboardText(found.text)
                                            : m_client->sendClipboardTextToTarget(found.text, entry.targetDeviceId);
        } else if (entry.targetDeviceId.isEmpty()) {
            m_client->sendClipboardImageFile(found.imagePath, found.mime, found.width, found.height,
                                             QByteArray::fromHex(found.sha256.toLatin1()));
        } else {
            m_client->sendClipboardImageFileToTarget(found.imagePath, found.mime, found.width, found.height,
                                                     QByteArray::fromHex(found.sha256.toLatin1()), entry.targetDeviceId);
        }
        return;
    }
    finishSendBatch();
}

void CloudClipboardController::finishActiveSend(int recipientCount, const QString &error) {
    if (m_activeSend.recordId.isEmpty())
        return;
    const QString recordId = m_activeSend.recordId;
    const SendOrigin origin = m_activeSend.origin;
    m_activeSend = {};
    ++m_sendBatchCompleted;
    if (!error.isEmpty()) ++m_sendBatchFailures;
    else if (recipientCount == 0) ++m_sendBatchNoRecipients;
    else ++m_sendBatchDelivered;
    emit transferFinished(recordId);
    if (!m_pendingSends.isEmpty()) { scheduleNextSend(); return; }
    finishSendBatch();
    if (!error.isEmpty() && origin == SendOrigin::Automatic)
        setTransferStatus(tr("Automatic clipboard send failed: %1").arg(error));
}

void CloudClipboardController::finishSendBatch() {
    if (!m_activeSend.recordId.isEmpty() || !m_pendingSends.isEmpty()) return;
    if (m_sendBatchTotal <= 0) setTransferStatus(QString());
    else if (m_sendBatchFailures > 0)
        setTransferStatus(tr("Queued %1 of %2 records; %3 failed.").arg(m_sendBatchDelivered).arg(m_sendBatchTotal).arg(m_sendBatchFailures));
    else if (m_sendBatchNoRecipients > 0)
        setTransferStatus(m_sendBatchDelivered > 0 ? tr("Queued %1 records; %2 had no recipients.").arg(m_sendBatchDelivered).arg(m_sendBatchNoRecipients) : noOtherDevicesMessage());
    else setTransferStatus(tr("Queued %1 selected records.").arg(m_sendBatchDelivered));
    m_sendBatchTotal = m_sendBatchCompleted = m_sendBatchDelivered = 0;
    m_sendBatchNoRecipients = m_sendBatchFailures = 0;
}

void CloudClipboardController::clearSendQueue() {
    m_activeSend = {};
    m_pendingSends.clear();
    m_sendBatchTotal = m_sendBatchCompleted = m_sendBatchDelivered = 0;
    m_sendBatchNoRecipients = m_sendBatchFailures = 0;
    m_nextSendScheduled = false;
}

bool CloudClipboardController::removeRecord(QString recordId) {
    int index = -1;
    for (int i = 0; i < items().size(); ++i) {
        if (items().at(i).id == recordId) {
            index = i;
            break;
        }
    }
    QString successor;
    for (int i = index + 1; i < items().size(); ++i) {
        if (items().at(i).id != recordId) {
            successor = items().at(i).id;
            break;
        }
    }
    if (successor.isEmpty()) {
        for (int i = index - 1; i >= 0; --i) {
            if (items().at(i).id != recordId) {
                successor = items().at(i).id;
                break;
            }
        }
    }
    return removeRecords({recordId}, successor);
}

bool CloudClipboardController::removeRecords(const QStringList &recordIds,
                                             const QString &preferredSelectionId) {
    QSet<QString> ids;
    for (const QString &id : recordIds) {
        if (!id.isEmpty() && m_store->lookup(id))
            ids.insert(id);
    }
    if (ids.isEmpty() || !m_store->removeRecords(ids.values()))
        return false;

    int removedQueued = 0;
    for (int i = m_pendingSends.size() - 1; i >= 0; --i) {
        if (ids.contains(m_pendingSends.at(i).recordId)) {
            m_pendingSends.removeAt(i);
            ++removedQueued;
        }
    }
    m_sendBatchTotal -= removedQueued;
    m_sendBatchTotal = qMax(m_sendBatchCompleted + (m_activeSend.recordId.isEmpty() ? 0 : 1),
                            m_sendBatchTotal);

    if (ids.contains(m_selectedRecordId)) {
        const QString next = !preferredSelectionId.isEmpty() && !ids.contains(preferredSelectionId) &&
                                     m_store->lookup(preferredSelectionId)
                                 ? preferredSelectionId
                                 : QString();
        if (m_selectedRecordId != next) {
            m_selectedRecordId = next;
            emit selectionChanged(next);
        }
    }
    emit changed();
    return true;
}

void CloudClipboardController::clear() {
    QStringList ids;
    for (const ClipboardHistoryRecord &known : items())
        ids.append(known.id);
    removeRecords(ids);
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
        const ClipboardHistoryRecord stored = m_store->addLocalText(capture.text);
        if (!stored.id.isEmpty()) {
            enqueueAutomaticRecord(stored, m_selectedTargetDeviceId);
            emit changed();
        }
        return;
    }
    const bool autoSendRequested = m_autoSendEnabled && m_client && m_client->isLoggedIn() &&
                                   targetDeviceIsAvailable();
    const QString accountDeviceId = m_client ? m_client->account().deviceId : QString();
    addCapturedImage(capture.image, autoSendRequested, m_selectedTargetDeviceId,
                     m_autoSendGeneration, accountDeviceId);
}

void CloudClipboardController::enqueueAutomaticRecord(const ClipboardHistoryRecord &record,
                                                       const QString &targetDeviceId) {
    if (!m_autoSendEnabled || !m_client || !m_client->isLoggedIn() ||
        (!targetDeviceId.isEmpty() && !isKnownTarget(targetDeviceId)))
        return;
    const QByteArray fingerprint = (record.kind == ClipboardRecordKind::Image ? QByteArrayLiteral("i")
                                                                        : QByteArrayLiteral("t")) +
                                   record.sha256.toLatin1();
    if (fingerprint == m_lastAutomaticFingerprint)
        return;
    m_lastAutomaticFingerprint = fingerprint;
    enqueueRecords({record.id}, targetDeviceId, SendOrigin::Automatic);
}

void CloudClipboardController::addCapturedImage(const QImage &image, bool autoSendRequested,
                                                const QString &targetDeviceId, quint64 autoSendGeneration,
                                                const QString &accountDeviceId) {
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this,
            [this, watcher, image, autoSendRequested, targetDeviceId, autoSendGeneration, accountDeviceId] {
        const QByteArray encoded = watcher->result();
        watcher->deleteLater();
        const ClipboardHistoryRecord stored = encoded.isEmpty()
                                                   ? ClipboardHistoryRecord()
                                                   : m_store->addLocalImage(encoded, QStringLiteral("image/png"),
                                                                            image.width(), image.height());
        if (!stored.id.isEmpty()) {
            if (autoSendRequested && m_autoSendEnabled &&
                autoSendGeneration == m_autoSendGeneration && m_client && m_client->isLoggedIn() &&
                m_client->account().deviceId == accountDeviceId)
                enqueueAutomaticRecord(stored, targetDeviceId);
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

bool CloudClipboardController::acceptsText(const QMimeData *mime, QString *text) {
    ClipboardCapture capture;
    if (!ClipboardHistoryStore::captureFromMimeData(mime, &capture) ||
        capture.kind != ClipboardRecordKind::Text) {
        return false;
    }
    if (text)
        *text = capture.text;
    return true;
}

bool CloudClipboardController::acceptsImage(const QMimeData *mime, QImage *image) {
    ClipboardCapture capture;
    if (!ClipboardHistoryStore::captureFromMimeData(mime, &capture) ||
        capture.kind != ClipboardRecordKind::Image) {
        return false;
    }
    if (image)
        *image = capture.image;
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
