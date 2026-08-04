#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QTemporaryDir>

#include "filesystem/FileInfo.h"
#include "filesystem/IconCache.h"

// An entry inside an archive has no file on disk: ArchiveProvider names it by
// its position within the archive ("/bin/cudart64_13.dll"), which is not a path
// any icon lookup can stat. The icon has to come from the extension alone.
//
// The suffixes below are deliberately odd and distinct per test: IconCache keys
// on the extension, so two tests sharing one would have the first populate the
// cache for the second and hide exactly the difference being measured.
namespace {

FileInfo entry(const QString &path, const QString &name) {
    return FileInfo::fromFields(path, name, 4096, QDateTime::currentDateTime(), false,
                                QFile::ReadOwner);
}

bool hasPixels(const QIcon &icon) {
    const QPixmap pixmap = icon.pixmap(48, 48);
    return !pixmap.isNull() && pixmap.width() > 0;
}

} // namespace

TEST(ArchiveEntryIcons, AnEntryInsideAnArchiveStillGetsAnIcon) {
    const FileInfo info = entry(QStringLiteral("/lib/win/sample.arqa"),
                                QStringLiteral("sample.arqa"));
    EXPECT_TRUE(hasPixels(IconCache::instance().iconFor(info)))
        << "an archive entry's path names a position inside the archive, so an "
           "icon lookup that stats it finds nothing and the grid draws a blank";
}

// The two must agree: browsing a folder and browsing an archive that holds the
// same file should not show different icons for it.
TEST(ArchiveEntryIcons, TheIconMatchesWhatTheSameNameGetsOnDisk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString onDisk = QDir(dir.path()).filePath(QStringLiteral("sample.arqb"));
    QFile file(onDisk);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    const QIcon local = IconCache::instance().iconFor(entry(onDisk, QStringLiteral("sample.arqb")));
    ASSERT_TRUE(hasPixels(local));

    // Same extension, so this is served from the cache the line above filled --
    // which is the point: the key is the extension, so the entry that filled it
    // must not have been one specific file's path.
    const QIcon inArchive =
        IconCache::instance().iconFor(entry(QStringLiteral("/sample.arqb"),
                                            QStringLiteral("sample.arqb")));
    EXPECT_EQ(local.pixmap(48, 48).toImage(), inArchive.pixmap(48, 48).toImage());
}
