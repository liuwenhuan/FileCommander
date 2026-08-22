#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <memory>

class QFile;

// Private, on-disk originals for cloud clipboard images. The item id is a
// canonical UUID, never a filename supplied by a peer. Entries expire after at
// most seven days and are bounded independently from the thumbnail cache.
class CloudClipboardImageCache {
public:
    static constexpr qint64 maximumImageBytes = 25LL * 1024 * 1024;
    static constexpr qint64 maximumCacheBytes = 512LL * 1024 * 1024;
    static constexpr int maximumAgeDays = 7;

    struct Entry {
        QString itemId;
        QString filePath;
        QString mimeType;
        qint64 size = 0;
        QByteArray sha256;
        QDateTime expiresAt;
    };

    // An empty directory means <Settings::configDir()>/cloud-clipboard/images.
    // Supplying a directory is intended for tests and an explicitly configured
    // FileShareServer; it is still rejected if it is a symlink.
    explicit CloudClipboardImageCache(const QString &directory = QString());

    static bool isSafeItemId(const QString &itemId);
    static QString normalizedItemId(const QString &itemId);

    QString directory() const;

    // Saves exactly the original bytes (never a decoded/re-encoded image).
    // An explicit expiry may shorten the seven-day retention period, never
    // lengthen it. mimeType must be safe for use as an HTTP Content-Type value.
    bool store(const QString &itemId, const QByteArray &original, const QString &mimeType,
               const QDateTime &expiresAt = QDateTime());

    bool lookup(const QString &itemId, Entry *entry = nullptr) const;
    std::unique_ptr<QFile> openRead(const QString &itemId, Entry *entry = nullptr) const;

    // Removes expired, malformed and orphaned entries, then evicts oldest files
    // until the cache is within maximumCacheBytes. Safe to call opportunistically.
    void cleanup() const;

private:
    QString dataPath(const QString &normalizedItemId) const;
    QString metadataPath(const QString &normalizedItemId) const;
    bool ensureDirectory() const;
    bool readEntry(const QString &normalizedItemId, Entry *entry, bool verifyHash) const;

    QString m_directory;
};
