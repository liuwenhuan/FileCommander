#pragma once

#include <memory>

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
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

    // Kept for NotepadPanel and MainWindow while their Task 6 migration is in
    // progress. The controller owns the default persistent store in this form.
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
    bool autoUpload() const { return false; }
    bool autoReceive() const { return false; }
    void setAgent(DeviceAgent *agent);
    void setDevices(const QVector<AccountDeviceInfo> &devices);
    QString deviceName(const QString &deviceId) const;

    void refresh();
    void selectRecord(const QString &recordId);
    bool copyRecordToClipboard(const QString &recordId);
    void sendRecord(const QString &recordId);
    bool removeRecord(const QString &recordId);
    void clear();

    // Transitional adapters for the pre-Task-6 panel. Each adapter remains
    // explicit: it first saves content locally and then sends that local record.
    void sendText(const QString &text);
    void sendCurrentClipboard();
    bool sendImageFromMimeData(const QMimeData *mime);
    bool sendStagedImage();
    bool hasStagedImage() const { return !m_stagedImage.isNull(); }
    void deleteItem(const QString &itemId) { removeRecord(itemId); }
    void requestThumbnail(const QString &) {}
    void requestOriginal(const CloudClipboardItem &) {}
    CloudClipboardItem item(const QString &) const { return {}; }
    void reportImageDownloadProgress(const QString &itemId, qint64 received, qint64 total);
    void completeImageDownload(const QString &itemId, const QByteArray &original);
    void failImageDownload(const QString &itemId, const QString &error);
    void setAutoUpload(bool) {}
    void confirmAutoUpload(bool) {}
    void setAutoReceive(bool) {}

    // Kept public and side-effect free for compatibility tests. New automatic
    // capture delegates to ClipboardHistoryStore::captureFromMimeData().
    static bool acceptsText(const QMimeData *mime, QString *text = nullptr);
    static bool acceptsImage(const QMimeData *mime, QImage *image = nullptr);

signals:
    void changed();
    void privacyWarningRequired(); // legacy panel compatibility; never emitted
    void selectionChanged(const QString &recordId);
    void transferStatusChanged(const QString &status);
    void transferProgress(const QString &recordId, qint64 completed, qint64 total);
    void imageSessionReady(const AccountSession &session); // legacy MainWindow signal
    void localImagePreview(const QImage &image);
    void localImagePublished();
    void imageDownloadProgress(const QString &itemId, qint64 received, qint64 total);
    void imageDownloadFailed(const QString &itemId, const QString &error);
    void imageCopied(const QString &itemId);

private slots:
    void onClipboardChanged();

private:
    void initialize(AccountClient *client, DeviceAgent *agent);
    void captureClipboard();
    void addCapturedImage(const QImage &image);
    void receiveDeliveries(const QVector<ClipboardDeliveryInfo> &deliveries);
    void finishDeliveryDownload(const QString &deliveryId, const QString &partPath);
    void setState(State state, const QString &error = QString());
    void setTransferStatus(const QString &status);
    bool hasOtherDevices() const;
    static QByteArray encodePng(const QImage &image);
    static QByteArray clipboardFingerprint(const QMimeData *mime);
    static bool isPrivateOrFileMime(const QMimeData *mime);

    std::unique_ptr<ClipboardHistoryStore> m_ownedStore;
    ClipboardHistoryStore *m_store = nullptr;
    AccountClient *m_client = nullptr;
    QClipboard *m_clipboard = nullptr;
    std::unique_ptr<QTemporaryDir> m_downloadDirectory;
    QVector<AccountDeviceInfo> m_devices;
    QHash<QString, QString> m_deviceNames;
    QHash<QString, ClipboardDeliveryInfo> m_pendingDeliveries;
    State m_state = State::SignedOut;
    QString m_error;
    QString m_transferStatus;
    QString m_selectedRecordId;
    QImage m_stagedImage;
    QByteArray m_ignoredClipboardFingerprint;
    QMetaObject::Connection m_agentConnection;
    QMetaObject::Connection m_agentReadyConnection;
    bool m_copyingToClipboard = false;
};
