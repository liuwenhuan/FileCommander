#include <gtest/gtest.h>

#include <QColor>
#include <QFile>
#include <QFileIconProvider>
#include <QImage>
#include <QPixmap>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shellapi.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include <cstring>
#include <cstdlib>

#include "FileInfo.h"
#include "IconCache.h"

#ifdef Q_OS_WIN
namespace {

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

QImage shellGenericFolderImage(int size) {
    SHFILEINFOW fileInfo = {};
    const UINT iconSize = size <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON;
    const DWORD_PTR ok = SHGetFileInfoW(
        L"folder", FILE_ATTRIBUTE_DIRECTORY, &fileInfo, sizeof(fileInfo),
        SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | iconSize);
    if (!ok || !fileInfo.hIcon)
        return {};
    QImage image = imageFromHIcon(fileInfo.hIcon, size);
    DestroyIcon(fileInfo.hIcon);
    return image;
}

double meanRgbDifference(const QImage &a, const QImage &b) {
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 255.0;
    QImage left = a.convertToFormat(QImage::Format_ARGB32);
    QImage right = b.convertToFormat(QImage::Format_ARGB32);
    quint64 total = 0;
    for (int y = 0; y < left.height(); ++y) {
        const QRgb *lp = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const QRgb *rp = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        for (int x = 0; x < left.width(); ++x) {
            total += static_cast<quint64>(std::abs(qRed(lp[x]) - qRed(rp[x])));
            total += static_cast<quint64>(std::abs(qGreen(lp[x]) - qGreen(rp[x])));
            total += static_cast<quint64>(std::abs(qBlue(lp[x]) - qBlue(rp[x])));
        }
    }
    return static_cast<double>(total) / static_cast<double>(left.width() * left.height() * 3);
}

} // namespace
#endif

TEST(IconCacheTest, ReturnsNonNullIconForFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("a.txt");
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.close();

    FileInfo info(path);
    QIcon icon = IconCache::instance().iconFor(info);
    EXPECT_FALSE(icon.isNull());
}

TEST(IconCacheTest, ReturnsNonNullIconForDirectory) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FileInfo info(dir.path());
    QIcon icon = IconCache::instance().iconFor(info);
    EXPECT_FALSE(icon.isNull());
}

TEST(IconCacheTest, DirectoryIconDoesNotUseDriveGlyph) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    IconCache::instance().setTint(QColor());
    const QImage folder =
        IconCache::instance().iconFor(FileInfo(dir.path())).pixmap(32, 32).toImage();
    const QImage drive = QFileIconProvider().icon(QFileIconProvider::Drive)
                             .pixmap(32, 32)
                             .toImage();

    ASSERT_FALSE(folder.isNull());
    ASSERT_FALSE(drive.isNull());
    EXPECT_NE(folder, drive);
}

#ifdef Q_OS_WIN
TEST(IconCacheTest, WindowsDirectoryIconUsesShellFolderGlyph) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    IconCache::instance().setTint(QColor());
    const QImage actual =
        IconCache::instance().iconFor(FileInfo(dir.path())).pixmap(32, 32).toImage();
    const QImage expected = shellGenericFolderImage(32);

    ASSERT_FALSE(actual.isNull());
    ASSERT_FALSE(expected.isNull());
    EXPECT_LT(meanRgbDifference(actual, expected), 2.0);
}

TEST(IconCacheTest, WindowsDirectoryIconFillsLargeThumbnailSlot) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    IconCache::instance().setTint(QColor());
    const QPixmap large =
        IconCache::instance().iconFor(FileInfo(dir.path())).pixmap(QSize(192, 192));

    ASSERT_FALSE(large.isNull());
    const qreal dpr = large.devicePixelRatio() > 0.0 ? large.devicePixelRatio() : 1.0;
    EXPECT_EQ(QSize(qRound(large.width() / dpr), qRound(large.height() / dpr)),
              QSize(192, 192));
}
#endif

TEST(IconCacheTest, ThemedIconLeavesOriginalUntouchedWithoutTint) {
    QPixmap source(16, 16);
    source.fill(QColor(130, 80, 40, 173));
    const QIcon raw(source);

    IconCache::instance().setTint(QColor());
    const QIcon result = IconCache::instance().themedIcon(raw);

    EXPECT_EQ(result.cacheKey(), raw.cacheKey());
}

TEST(IconCacheTest, ThemedIconUsesConfiguredTintAndLeavesAlphaIntact) {
    QPixmap source(16, 16);
    source.fill(QColor(130, 80, 40, 173));
    const QIcon raw(source);

    IconCache::instance().setTint(QColor(0, 255, 0), 0);
    const QPixmap tinted = IconCache::instance().themedIcon(raw).pixmap(source.size());
    IconCache::instance().setTint(QColor());

    ASSERT_FALSE(tinted.isNull());
    const QColor pixel = tinted.toImage().pixelColor(0, 0);
    EXPECT_EQ(pixel.red(), 0);
    EXPECT_GT(pixel.green(), 0);
    EXPECT_EQ(pixel.blue(), 0);
    EXPECT_EQ(pixel.alpha(), 173);
}

TEST(IconCacheTest, SameExtensionReusesCachedIcon) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QFile a(dir.filePath("a.txt"));
    a.open(QIODevice::WriteOnly);
    a.close();
    QFile b(dir.filePath("b.txt"));
    b.open(QIODevice::WriteOnly);
    b.close();

    QIcon iconA = IconCache::instance().iconFor(FileInfo(dir.filePath("a.txt")));
    QIcon iconB = IconCache::instance().iconFor(FileInfo(dir.filePath("b.txt")));
    // Both .txt files should resolve through the same cache entry.
    EXPECT_FALSE(iconA.isNull());
    EXPECT_FALSE(iconB.isNull());
    EXPECT_EQ(iconA.cacheKey(), iconB.cacheKey());
}

// QIcon::pixmap() never scales a bitmap UP. A system file-type icon often ships
// only 16x16, so an archive or an .exe came back at 16px however large the
// thumbnail grid was and sat as a speck beside folders, which are drawn from
// scalable SVGs and filled their cells.
TEST(IconPixmapSizeTest, EnlargesAnIconThatOnlyShipsASmallBitmap) {
    QPixmap tiny(16, 16);
    tiny.fill(Qt::red);
    const QIcon icon = QIcon(tiny);

    // What the old code did, kept here as the contrast: asking for 96 gets 16.
    EXPECT_EQ(icon.pixmap(QSize(96, 96)).width(), 16)
        << "QIcon started enlarging bitmaps; this helper may no longer be needed";

    const QPixmap scaled = IconCache::pixmapOfSize(icon, 96, 1.0);
    EXPECT_EQ(scaled.width(), 96);
    EXPECT_EQ(scaled.height(), 96);
}

TEST(IconPixmapSizeTest, LeavesAnIconThatIsAlreadyBigEnoughAlone) {
    QPixmap large(128, 128);
    large.fill(Qt::blue);
    const QIcon icon = QIcon(large);

    const QPixmap result = IconCache::pixmapOfSize(icon, 96, 1.0);
    // Whatever QIcon hands back for a request of 96 is already at least 96, so
    // it must not be resampled -- rescaling a large bitmap only softens it.
    EXPECT_GE(result.width(), 96);
}

TEST(IconPixmapSizeTest, HonoursTheDevicePixelRatio) {
    QPixmap tiny(16, 16);
    tiny.fill(Qt::green);
    const QIcon icon = QIcon(tiny);

    const QPixmap scaled = IconCache::pixmapOfSize(icon, 48, 2.0);
    EXPECT_DOUBLE_EQ(scaled.devicePixelRatio(), 2.0);
    // 48 logical pixels at 2x is 96 device pixels; a caller dividing by the
    // ratio has to land back on 48.
    EXPECT_EQ(qRound(scaled.width() / scaled.devicePixelRatio()), 48);
}

TEST(IconPixmapSizeTest, ANullIconStaysNull) {
    EXPECT_TRUE(IconCache::pixmapOfSize(QIcon(), 96, 1.0).isNull());
    QPixmap p(16, 16);
    p.fill(Qt::red);
    EXPECT_TRUE(IconCache::pixmapOfSize(QIcon(p), 0, 1.0).isNull());
}
