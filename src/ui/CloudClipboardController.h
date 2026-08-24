#pragma once

#include <memory>

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "account/AccountClient.h"
#include "account/ClipboardHistoryStore.h"

class QClipboard;
class QMimeData;
class Settings;
class DeviceAgent;
class QTemporaryDir;

// UI policy for the private local clipboard history and explicit account
// deliveries. AccountClient owns all network transport; this class never
// publishes an observed system clipboard change or applies a delivery to it.
class CloudClipboardController : public QObject {
    Q_OBJECT

public:
    enum class State { SignedOut, Loading, Empty, Ready, Error };

    // The controller owns the default persistent store in this form.
    explicit CloudClipboardController(Settings &settings, AccountClient *client,
                                     DeviceAgent *agent = nullptr, QObject *parent = nullptr);
    // Injectable store keeps controller behavior independently testable.
    explicit CloudClipboardController(ClipboardHistoryStore &store, AccountClient *client,
                                     DeviceAgent *agent = nullptr, QObject *parent = nullptr);

    State state() const;
    QString error() const;
    const QVector<ClipboardHistoryRecord> &items() const;
    ClipboardHistoryRecord record(const QString &id) const;
    QString selectedRecordId() const;
    void setAgent(DeviceAgent *agent);
    void setDevices(const QVector<AccountDeviceInfo> &devices);
    const QVector<AccountDeviceInfo> &devices() const;
    QString deviceName(const QString &deviceId) const;
    bool autoSendEnabled() const;
    void setAutoSendEnabled(bool enabled);
    QString selectedTargetDeviceId() const;
    void setSelectedTargetDeviceId(const QString &deviceId);
    bool targetDeviceIsAvailable() const;

    void refresh();
    void selectRecord(const QString &recordId);
    bool copyRecordToClipboard(const QString &recordId);
    void sendRecord(const QString &recordId, const QString &targetDeviceId = QString());
    void sendRecords(const QStringList &recordIds, const QString &targetDeviceId = QString());
    bool removeRecord(QString recordId);
    bool removeRecords(const QStringList &recordIds, const QString &preferredSelectionId = QString());
    void clear();

    // Capture admission is public for focused policy tests.
    static bool acceptsText(const QMimeData *mime, QString *text = nullptr);
    static bool acceptsImage(const QMimeData *mime, QImage *image = nullptr);

signals:
    void changed();
    void devicesChanged();
    void autoSendChanged(bool enabled);
    void selectedTargetDeviceChanged(const QString &deviceId);
    void selectionChanged(const QString &recordId);
    void transferStatusChanged(const QString &status);
    void transferProgress(const QString &recordId, qint64 completed, qint64 total);
    void transferFinished(const QString &recordId);
    void transferReset();

private slots:
    void onClipboardChanged();

private:
    enum class DeliveryState { Downloading, Acknowledging, AwaitingAcknowledgement };
    struct DeliveryEntry {
        ClipboardDeliveryInfo info;
        DeliveryState state = DeliveryState::Downloading;
    };

    enum class SendOrigin { Manual, Automatic };
    struct PendingSend {
        QString recordId;
        QString targetDeviceId;
        SendOrigin origin = SendOrigin::Manual;
    };

    void initialize(AccountClient *client, DeviceAgent *agent);
    void captureClipboard();
    void addCapturedImage(const QImage &image, bool autoSendRequested,
                          const QString &targetDeviceId, quint64 autoSendGeneration,
                          const QString &accountDeviceId);
    void receiveDeliveries(const QVector<ClipboardDeliveryInfo> &deliveries);
    void finishDeliveryDownload(const QString &deliveryId, const QString &partPath);
    void setState(State state, const QString &error = QString());
    void setTransferStatus(const QString &status);
    bool hasOtherDevices() const;
    bool isKnownTarget(const QString &deviceId) const;
    bool enqueueRecords(const QStringList &recordIds, const QString &targetDeviceId, SendOrigin origin);
    void enqueueAutomaticRecord(const ClipboardHistoryRecord &record,
                                const QString &targetDeviceId);
    void startNextSend();
    void finishActiveSend(int recipientCount, const QString &error = QString());
    void finishSendBatch();
    void scheduleNextSend();
    void clearSendQueue();
    static QByteArray encodePng(const QImage &image);
    static QByteArray clipboardFingerprint(const QMimeData *mime);

    std::unique_ptr<ClipboardHistoryStore> m_ownedStore;
    ClipboardHistoryStore *m_store = nullptr;
    Settings *m_settings = nullptr;
    AccountClient *m_client = nullptr;
    QClipboard *m_clipboard = nullptr;
    std::unique_ptr<QTemporaryDir> m_downloadDirectory;
    QVector<AccountDeviceInfo> m_devices;
    QHash<QString, QString> m_deviceNames;
    QHash<QString, DeliveryEntry> m_deliveries;
    QSet<QString> m_completedDeliveryIds;
    State m_state = State::SignedOut;
    QString m_error;
    QString m_transferStatus;
    QString m_selectedRecordId;
    PendingSend m_activeSend;
    QVector<PendingSend> m_pendingSends;
    bool m_autoSendEnabled = false;
    quint64 m_autoSendGeneration = 0;
    QString m_selectedTargetDeviceId;
    QByteArray m_lastAutomaticFingerprint;
    int m_sendBatchTotal = 0;
    int m_sendBatchCompleted = 0;
    int m_sendBatchDelivered = 0;
    int m_sendBatchNoRecipients = 0;
    int m_sendBatchFailures = 0;
    bool m_nextSendScheduled = false;
    QByteArray m_ignoredClipboardFingerprint;
    QMetaObject::Connection m_agentConnection;
    QMetaObject::Connection m_agentReadyConnection;
    bool m_copyingToClipboard = false;
};
