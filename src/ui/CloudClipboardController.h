#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include "account/AccountClient.h"

class QClipboard;
class QImage;
class QMimeData;
class QTimer;
class Settings;
class DeviceAgent;

// Keeps the account-backed clipboard and the platform clipboard separate from
// the popup. It owns only UI-side policy: server requests stay in AccountClient.
class CloudClipboardController : public QObject {
    Q_OBJECT

public:
    enum class State { SignedOut, Loading, Empty, Ready, Error };

    explicit CloudClipboardController(Settings &settings, AccountClient *client,
                                     DeviceAgent *agent = nullptr, QObject *parent = nullptr);

    State state() const;
    QString error() const;
    const QVector<CloudClipboardItem> &items() const;
    bool autoUpload() const;
    bool autoReceive() const;
    void setAgent(DeviceAgent *agent);
    void setDevices(const QVector<AccountDeviceInfo> &devices);
    QString deviceName(const QString &deviceId) const;

    void refresh();
    void sendText(const QString &text);
    void sendCurrentClipboard();
    bool sendImageFromMimeData(const QMimeData *mime);
    bool sendStagedImage();
    bool hasStagedImage() const { return !m_stagedImage.isNull(); }
    void deleteItem(const QString &itemId);
    void clear();
    void requestThumbnail(const QString &itemId);
    void requestOriginal(const CloudClipboardItem &item);
    CloudClipboardItem item(const QString &itemId) const;
    void reportImageDownloadProgress(const QString &itemId, qint64 received, qint64 total);
    void completeImageDownload(const QString &itemId, const QByteArray &original);
    void failImageDownload(const QString &itemId, const QString &error);
    void setAutoUpload(bool enabled);
    void confirmAutoUpload(bool accepted);
    void setAutoReceive(bool enabled);

    // Kept public and side-effect free so focused tests can cover clipboard
    // admission without relying on the desktop clipboard implementation.
    static bool acceptsText(const QMimeData *mime, QString *text = nullptr);
    static bool acceptsImage(const QMimeData *mime, QImage *image = nullptr);

signals:
    void changed();
    void privacyWarningRequired();
    void imageSessionReady(const AccountSession &session);
    void localImagePreview(const QImage &image);
    void localImagePublished();
    void imageDownloadProgress(const QString &itemId, qint64 received, qint64 total);
    void imageDownloadFailed(const QString &itemId, const QString &error);
    void imageCopied(const QString &itemId);

private slots:
    void onClipboardChanged();
    void publishPendingClipboard();

private:
    static QByteArray digest(const QByteArray &bytes, char kind);
    static QByteArray encodePng(const QImage &image);
    static QByteArray thumbnail(const QImage &image);
    static bool isPrivateOrFileMime(const QMimeData *mime);
    bool publishImage(const QImage &image);
    void setState(State state, const QString &error = QString());
    void acceptUpdate(const CloudClipboardUpdate &update);
    void acceptItem(const CloudClipboardItem &item);
    void applyLatestRemoteText();
    QByteArray currentClipboardDigest() const;

    Settings &m_settings;
    AccountClient *m_client = nullptr;
    QClipboard *m_clipboard = nullptr;
    QTimer *m_debounce = nullptr;
    QVector<CloudClipboardItem> m_items;
    QHash<QString, QString> m_deviceNames;
    State m_state = State::SignedOut;
    QString m_error;
    QByteArray m_observedDigest;
    QByteArray m_refreshDigest;
    QByteArray m_pendingDigest;
    QHash<QString, QPair<QByteArray, QString>> m_pendingOriginals; // sha256 -> bytes,mime
    QImage m_stagedImage;
    QString m_pendingDownloadId;
    QString m_pendingDeleteId;
    QMetaObject::Connection m_agentConnection;
    bool m_applyingRemote = false;
};
