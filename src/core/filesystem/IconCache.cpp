#include "IconCache.h"

#include <QFileIconProvider>
#include <QImage>
#include <QPixmap>
#include <QVector>

#include "FileInfo.h"
#include "theme/Phosphor.h"

namespace {
// Cost is 1 per entry; 200 covers a wide spread of file extensions plus
// the directory icon without holding onto icons for extensions the user
// hasn't seen recently.
constexpr int kCacheBudget = 200;

// Sizes to materialise when a tinted icon is rebuilt. availableSizes() answers
// this for a themed (pixmap) icon, but an SVG icon is scalable and reports
// none, so these are the fallback rungs -- the list view and the thumbnail grid
// between them ask for everything from 16 up.
constexpr int kTintSizes[] = {16, 22, 24, 32, 48, 64, 128, 256};
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
    QIcon icon = info.isDir() ? provider.icon(QFileIconProvider::Folder)
                              : provider.icon(QFileInfo(info.path()));
    if (m_tint.isValid())
        icon = tinted(icon);

    m_cache.insert(key, new QIcon(icon));
    return icon;
}

void IconCache::setTint(const QColor &tint, int blockPixels) {
    // Compare by value: an invalid QColor equals another invalid one, so
    // clearing twice is a no-op rather than a needless cache flush.
    if (m_tint == tint && m_tint.isValid() == tint.isValid() && m_blockPixels == blockPixels)
        return;
    m_tint = tint;
    m_blockPixels = blockPixels;
    m_cache.clear(); // entries were built under the old tint/grid
}

QIcon IconCache::tinted(const QIcon &icon) const {
    QList<QSize> sizes = icon.availableSizes();
    if (sizes.isEmpty()) {
        for (int s : kTintSizes)
            sizes.append(QSize(s, s));
    }

    QIcon out;
    for (const QSize &size : sizes) {
        const QPixmap src = icon.pixmap(size);
        if (src.isNull())
            continue;
        // The same luma-to-phosphor map thumbnails, previews and video use --
        // but on the coarser ICON grid. A glyph carries one idea and survives a
        // blocky raster; a photograph does not, which is why content has its
        // own, finer block (see fc::kIconBlockLogical vs kContentBlockLogical).
        out.addPixmap(fc::tintedPixmap(src, m_tint, m_blockPixels));
    }
    return out.isNull() ? icon : out;
}
