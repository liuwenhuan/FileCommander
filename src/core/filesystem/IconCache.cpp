#include "IconCache.h"

#include <QFileIconProvider>

#include "FileInfo.h"

namespace {
// Cost is 1 per entry; 200 covers a wide spread of file extensions plus
// the directory icon without holding onto icons for extensions the user
// hasn't seen recently.
constexpr int kCacheBudget = 200;
} // namespace

IconCache &IconCache::instance() {
    static IconCache cache;
    return cache;
}

IconCache::IconCache() : m_cache(kCacheBudget) {}

QIcon IconCache::iconFor(const FileInfo &info) {
    const QString key = info.isDir() ? QStringLiteral("dir")
                         : info.suffix().isEmpty()
                             ? QStringLiteral("file:noext")
                             : QStringLiteral("file:") + info.suffix().toLower();

    if (QIcon *cached = m_cache.object(key))
        return *cached;

    static QFileIconProvider provider;
    const QIcon icon = info.isDir() ? provider.icon(QFileIconProvider::Folder)
                                     : provider.icon(QFileInfo(info.path()));

    m_cache.insert(key, new QIcon(icon));
    return icon;
}
