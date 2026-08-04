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

// Windows' jumbo image list hands back a 256x256 slot for every icon, even one
// that never shipped a 256px variant -- and it does not centre the smaller
// bitmap in it, it drops it in the TOP-LEFT corner and leaves the rest
// transparent. Measured for .rar (7-Zip's icon, no 256 variant): a 256 canvas
// with 44x34 of ink at (2,7), which the grid then drew as a small badge pinned
// to a corner of a large tile.
//
// Cropping that back to a square around the ink turns the lie into the truth --
// the icon reports the size it actually has, and the thumbnail delegate's
// upscale (which only fires on a pixmap SMALLER than the box) can then grow it.
//
// Only for the large rungs: at 16 or 32 there is not enough room for the test to
// mean anything. A real icon of this size is never confined to one quadrant --
// .zip's genuine 256 measured 227x176 at (13,34) -- so this leaves them alone.
QImage cropPaddedShellIcon(const QImage &image, int size) {
    if (size < 64 || image.isNull())
        return image;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8) {
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < 0 || maxY < 0)
        return image; // fully transparent; nothing to say about it
    const int extent = qMax(maxX, maxY) + 1;
    if (extent > size / 2)
        return image; // fills the canvas: a real icon of this size
    // Round up to the rung the source almost certainly is, so the crop keeps
    // the artwork's own padding rather than shrink-wrapping the ink.
    int side = size;
    for (int rung : {32, 48, 64, 128}) {
        if (extent <= rung) {
            side = rung;
            break;
        }
    }
    return image.copy(0, 0, side, side);
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
    return QPixmap::fromImage(cropPaddedShellIcon(image, size));
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

QIcon IconCache::iconFor(const FileInfo &info) {
    const QString key = info.isDir() ? QStringLiteral("dir")
                         : info.suffix().isEmpty()
                             ? QStringLiteral("file:noext")
                             : QStringLiteral("file:") + info.suffix().toLower();

    if (QIcon *cached = m_cache.object(key))
        return *cached;

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
    icon = tinted(icon, m_fileIconTint);

    m_cache.insert(key, new QIcon(icon));
    return icon;
}

QIcon IconCache::systemIconForPath(const QString &path) {
#ifdef Q_OS_WIN
    if (path.isEmpty())
        return {};
    const QString key = QStringLiteral("path:") + path;
    if (QIcon *cached = m_cache.object(key))
        return *cached;

    // No SHGFI_USEFILEATTRIBUTES here, unlike everywhere else in this file:
    // the point is to ask about this drive, not about drives in general.
    QIcon icon;
    const QString native = QDir::toNativeSeparators(path);
    const int sizes[] = {16, 32, 48, 256};
    for (int size : sizes) {
        SHFILEINFOW fileInfo = {};
        if (!SHGetFileInfoW(reinterpret_cast<const wchar_t *>(native.utf16()), 0, &fileInfo,
                            sizeof(fileInfo), SHGFI_SYSICONINDEX))
            return {};
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
        // Same jumbo padding as the file icons: a drive whose icon has no 256px
        // variant comes back as a corner of an otherwise empty canvas.
        if (!image.isNull())
            icon.addPixmap(QPixmap::fromImage(cropPaddedShellIcon(image, size)));
    }
    if (icon.availableSizes().isEmpty())
        return {};
    icon = tinted(icon, m_fileIconTint);
    m_cache.insert(key, new QIcon(icon));
    return icon;
#else
    Q_UNUSED(path);
    return {};
#endif
}

QIcon IconCache::glyphIcon(const QString &resourcePath) {
    const QString key = QStringLiteral("glyph:") + resourcePath;
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
    if (sameTint(m_glyphTint, tint))
        return;
    m_glyphTint = tint;
    // themedIcon() results are not cached here (their callers hold them), but
    // clearing costs nothing on a theme switch and keeps the two setters alike.
    m_cache.clear();
}

void IconCache::setFileIconTint(const QColor &tint, int blockPixels) {
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
