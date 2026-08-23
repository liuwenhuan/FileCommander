#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QString>
#include <QVector>

class QMimeData;

enum class ClipboardRecordOrigin { Local, Incoming };
enum class ClipboardRecordKind { Text, Image };

struct ClipboardHistoryRecord {
    QString id;
    ClipboardRecordOrigin origin = ClipboardRecordOrigin::Local;
    ClipboardRecordKind kind = ClipboardRecordKind::Text;
    QString text;
    QString imagePath;
    QString mime;
    QString sha256;
    QString sourceDeviceId;
    QString sourceDeviceName;
    qint64 size = 0;
    int width = 0;
    int height = 0;
    QDateTime created;
};

struct ClipboardCapture {
    ClipboardRecordKind kind = ClipboardRecordKind::Text;
    QString text;
    QImage image;
    QString mime;
};

// Private, persistent local clipboard history. Image paths are derived from the
// record id and deliberately never appear in the on-disk manifest.
class ClipboardHistoryStore {
public:
    static constexpr int maximumRecords = 50;
    static constexpr int maximumTextBytes = 64 * 1024;
    static constexpr qint64 maximumImageBytes = 25LL * 1024 * 1024;
    static constexpr qint64 maximumPixels = 40LL * 1000 * 1000;

    // An empty config directory uses Settings::configDir(). A supplied directory
    // is a configuration root, with history stored below cloud-clipboard/history.
    explicit ClipboardHistoryStore(const QString &configDirectory = QString());

    static bool captureFromMimeData(const QMimeData *mime, ClipboardCapture *capture = nullptr);

    bool load();
    const QVector<ClipboardHistoryRecord> &records() const;
    ClipboardHistoryRecord addLocalText(const QString &text);
    ClipboardHistoryRecord addLocalImage(const QByteArray &encoded, const QString &mime,
                                         int width, int height);
    ClipboardHistoryRecord addIncomingText(const QString &text, const QString &sourceDeviceId,
                                           const QString &sourceDeviceName,
                                           const QDateTime &created = QDateTime());
    ClipboardHistoryRecord addIncomingImageFile(const QString &sourcePath, const QString &mime,
                                                qint64 size, int width, int height,
                                                const QString &sha256,
                                                const QString &sourceDeviceId,
                                                const QString &sourceDeviceName,
                                                const QDateTime &created = QDateTime());
    bool remove(const QString &id);
    bool lookup(const QString &id, ClipboardHistoryRecord *record = nullptr) const;

private:
    QString directory() const;
    QString imagesDirectory() const;
    QString legacyImagesDirectory() const;
    QString manifestPath() const;
    QString imagePathFor(const QString &id) const;
    bool ensureDirectories() const;
    void removeLegacyImagesDirectory() const;
    bool saveManifest(const QVector<ClipboardHistoryRecord> &records) const;
    bool persist(const QVector<ClipboardHistoryRecord> &records);
    void cleanupOrphanImages() const;
    ClipboardHistoryRecord addText(const QString &text, ClipboardRecordOrigin origin,
                                   const QString &sourceDeviceId, const QString &sourceDeviceName,
                                   const QDateTime &created);
    ClipboardHistoryRecord addImage(const QByteArray &encoded, ClipboardRecordOrigin origin,
                                    const QString &sourceDeviceId, const QString &sourceDeviceName,
                                    const QDateTime &created);

    QString m_configDirectory;
    QVector<ClipboardHistoryRecord> m_records;
    bool m_loaded = false;
};
