#include "IconCache.h"

#include <QDir>
#include <QFileIconProvider>
#include <QImage>
#include <QPixmap>
#include <QVector>

#include <cstring>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

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

#ifdef Q_OS_WIN
QImage imageFromHIcon(HICON icon, int size) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!screen || !dc || !bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        if (screen)
            ReleaseDC(nullptr, screen);
        return {};
    }

    HGDIOBJ old = SelectObject(dc, bitmap);
    std::memset(bits, 0, static_cast<size_t>(size * size * 4));
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    QImage image(static_cast<uchar *>(bits), size, size, QImage::Format_ARGB32);
    QImage copy = image.copy();
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return copy;
}

// SHGFI_USEFILEATTRIBUTES means "answer from the name and the attributes, do
// not go to disk", so `name` is a stand-in rather than a real path -- which is
// the whole point here: the caller has an extension, not a file.
QPixmap shellPixmap(const QString &name, DWORD attributes, int size) {
    SHFILEINFOW fileInfo = {};
    const DWORD_PTR ok = SHGetFileInfoW(
        reinterpret_cast<const wchar_t *>(name.utf16()), attributes, &fileInfo, sizeof(fileInfo),
        SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
    if (!ok)
        return {};

    const int imageListSize = size <= 16   ? SHIL_SMALL
                              : size <= 32 ? SHIL_LARGE
                              : size <= 48 ? SHIL_EXTRALARGE
                                           : SHIL_JUMBO;
    IImageList *imageList = nullptr;
    const HRESULT listResult = SHGetImageList(
        imageListSize, __uuidof(IImageList), reinterpret_cast<void **>(&imageList));
    if (FAILED(listResult) || !imageList)
        return {};

    HICON shellIcon = nullptr;
    const HRESULT iconResult = imageList->GetIcon(fileInfo.iIcon, ILD_TRANSPARENT, &shellIcon);
    imageList->Release();
    if (FAILED(iconResult) || !shellIcon)
        return {};

    QImage image = imageFromHIcon(shellIcon, size);
    DestroyIcon(shellIcon);
    if (image.isNull())
        return {};
    return QPixmap::fromImage(IconCache::cropPaddedIcon(image, size));
}

QIcon shellIconFor(const QString &name, DWORD attributes) {
    QIcon icon;
    const int sizes[] = {16, 32, 48, 256};
    for (int size : sizes) {
        QPixmap pixmap = shellPixmap(name, attributes, size);
        if (!pixmap.isNull())
            icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon shellFolderIcon() {
    return shellIconFor(QStringLiteral("folder"), FILE_ATTRIBUTE_DIRECTORY);
}
#endif
} // namespace

IconCache &IconCache::instance() {
    static IconCache cache;
    return cache;
}

IconCache::IconCache() : m_cache(kCacheBudget) {}

QImage IconCache::cropPaddedIcon(const QImage &image, int size) {
    if (size < 64 || image.isNull())
        return image;
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < 0 || maxY < 0)
        return image; // fully transparent; nothing to say about it

    const int inkWidth = maxX - minX + 1;
    const int inkHeight = maxY - minY + 1;
    const int longest = qMax(inkWidth, inkHeight);
    if (longest * 4 >= size * 3)
        return image; // fills the canvas: a real icon of this size

    // A square around the ink, centred on it, with a small margin so the glyph
    // is not shrink-wrapped -- icons are drawn with a little air around them and
    // removing all of it makes them look bigger than everything else.
    const int side = qMin(size, longest + longest / 8 + 2);
    const int cx = (minX + maxX) / 2;
    const int cy = (minY + maxY) / 2;
    const int left = qBound(0, cx - side / 2, image.width() - side);
    const int top = qBound(0, cy - side / 2, image.height() - side);
    return image.copy(left, top, side, side);
}


QIcon IconCache::iconFor(const FileInfo &info) {
    const QString key = info.isDir() ? QStringLiteral("dir")
                         : info.suffix().isEmpty()
                             ? QStringLiteral("file:noext")
                             : QStringLiteral("file:") + info.suffix().toLower();

    {
        QMutexLocker locker(&m_mutex);
        if (QIcon *cached = m_cache.object(key))
            return *cached;
    }

    static QFileIconProvider provider;
    QIcon icon;
    if (info.isDir()) {
#ifdef Q_OS_WIN
        icon = shellFolderIcon();
#endif
        if (icon.isNull())
            icon = provider.icon(QFileIconProvider::Folder);
    } else {
        // Resolved from the EXTENSION, never from info.path(). An entry inside
        // an archive is named by its position in the archive, not by anything
        // on disk, and asking the shell about a path that does not exist
        // answers with an icon that has no pixmaps -- which is a blank cell in
        // the grid. QFileIconProvider does not rescue it either: its
        // generic-file fallback is guarded by isFile(), which a non-existent
        // path fails.
        //
        // Nothing is lost by ignoring the path. The cache is keyed by extension
        // already, so a per-file icon (an .exe carrying its own) never survived
        // the first lookup anyway -- resolving by extension just makes the icon
        // agree with the key instead of depending on which file was seen first.
        const QString stand_in = info.suffix().isEmpty()
                                     ? QStringLiteral("file")
                                     : QStringLiteral("file.") + info.suffix().toLower();
#ifdef Q_OS_WIN
        icon = shellIconFor(stand_in, FILE_ATTRIBUTE_NORMAL);
#else
        icon = provider.icon(QFileInfo(stand_in));
#endif
        if (icon.availableSizes().isEmpty())
            icon = provider.icon(QFileIconProvider::File);
    }
    QMutexLocker locker(&m_mutex);
    icon = tinted(icon, m_fileIconTint);
    m_cache.insert(key, new QIcon(icon));
    return icon;
}

QIcon IconCache::systemIconForPath(const QString &path) const {
    if (path.isEmpty())
        return {};
    QMutexLocker locker(&m_mutex);
    QIcon *cached = m_cache.object(QStringLiteral("path:") + path);
    return cached ? *cached : QIcon();
}

bool IconCache::hasSystemIconLookup() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void IconCache::warmSystemIconForPath(const QString &path) {
#ifdef Q_OS_WIN
    if (path.isEmpty())
        return;

    // SHGetImageList hands back an IImageList, i.e. COM -- and this runs on a
    // worker, which Qt does not initialise COM on. Without this the call does
    // not fail cleanly: the task never returns, and the process then hangs at
    // exit waiting for it (measured: a test binary that printed its results and
    // never quit). Apartment-threaded to match what the shell expects.
    struct ComScope {
        bool owned = false;
        ComScope() {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            // S_FALSE means this thread already had COM up -- someone else owns
            // the uninitialise in that case.
            owned = hr == S_OK;
        }
        ~ComScope() {
            if (owned)
                CoUninitialize();
        }
    } com;
    const QString key = QStringLiteral("path:") + path;
    {
        QMutexLocker locker(&m_mutex);
        if (m_cache.object(key))
            return;
    }

    // No SHGFI_USEFILEATTRIBUTES here, unlike everywhere else in this file:
    // the point is to ask about this drive, not about drives in general. That
    // is also why this must not run on the GUI thread -- the question goes to
    // the volume.
    QIcon icon;
    const QString native = QDir::toNativeSeparators(path);
    const int sizes[] = {16, 32, 48, 256};
    for (int size : sizes) {
        SHFILEINFOW fileInfo = {};
        if (!SHGetFileInfoW(reinterpret_cast<const wchar_t *>(native.utf16()), 0, &fileInfo,
                            sizeof(fileInfo), SHGFI_SYSICONINDEX))
            return;
        const int imageListSize = size <= 16   ? SHIL_SMALL
                                  : size <= 32 ? SHIL_LARGE
                                  : size <= 48 ? SHIL_EXTRALARGE
                                               : SHIL_JUMBO;
        IImageList *imageList = nullptr;
        if (FAILED(SHGetImageList(imageListSize, __uuidof(IImageList),
                                  reinterpret_cast<void **>(&imageList))) ||
            !imageList)
            continue;
        HICON shellIcon = nullptr;
        const HRESULT iconResult =
            imageList->GetIcon(fileInfo.iIcon, ILD_TRANSPARENT, &shellIcon);
        imageList->Release();
        if (FAILED(iconResult) || !shellIcon)
            continue;
        const QImage image = imageFromHIcon(shellIcon, size);
        DestroyIcon(shellIcon);
        if (!image.isNull())
            icon.addPixmap(QPixmap::fromImage(cropPaddedIcon(image, size)));
    }
    if (icon.availableSizes().isEmpty())
        return;

    QMutexLocker locker(&m_mutex);
    m_cache.insert(key, new QIcon(tinted(icon, m_fileIconTint)));
#else
    Q_UNUSED(path);
#endif
}

QIcon IconCache::glyphIcon(const QString &resourcePath) {
    const QString key = QStringLiteral("glyph:") + resourcePath;
    QMutexLocker locker(&m_mutex);
    if (QIcon *cached = m_cache.object(key))
        return *cached;

    const QIcon source(resourcePath);
    QIcon result = source;
    if (m_glyphTint.isValid()) {
        QIcon out;
        for (int size : kTintSizes) {
            const QPixmap pixmap = source.pixmap(size, size);
            if (pixmap.isNull())
                continue;
            QImage image = pixmap.toImage();
            fc::flattenToTint(image, m_glyphTint);
            out.addPixmap(QPixmap::fromImage(image));
        }
        if (!out.availableSizes().isEmpty())
            result = out;
    }
    m_cache.insert(key, new QIcon(result));
    return result;
}

QIcon IconCache::themedIcon(const QIcon &icon) const {
    return tinted(icon, m_glyphTint);
}

// Compare by value: an invalid QColor equals another invalid one, so clearing
// twice is a no-op rather than a needless cache flush.
static bool sameTint(const QColor &a, const QColor &b) {
    return a == b && a.isValid() == b.isValid();
}

void IconCache::setGlyphTint(const QColor &tint) {
    QMutexLocker locker(&m_mutex);
    if (sameTint(m_glyphTint, tint))
        return;
    m_glyphTint = tint;
    // themedIcon() results are not cached here (their callers hold them), but
    // clearing costs nothing on a theme switch and keeps the two setters alike.
    m_cache.clear();
}

void IconCache::setFileIconTint(const QColor &tint, int blockPixels) {
    QMutexLocker locker(&m_mutex);
    if (sameTint(m_fileIconTint, tint) && m_blockPixels == blockPixels)
        return;
    m_fileIconTint = tint;
    m_blockPixels = blockPixels;
    m_cache.clear(); // entries were built under the old tint/grid
}

QIcon IconCache::tinted(const QIcon &icon, const QColor &tint) const {
    if (!tint.isValid())
        return icon;
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
        // The same luma-to-phosphor map thumbnails, previews and video use.
        out.addPixmap(fc::tintedPixmap(src, tint, m_blockPixels));
    }
    return out.isNull() ? icon : out;
}
