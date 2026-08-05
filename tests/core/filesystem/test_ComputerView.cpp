#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "filesystem/ComputerCatalog.h"
#include "filesystem/ComputerProvider.h"
#include "filesystem/FileSystemModel.h"

namespace {

ComputerEntry makeEntry(ComputerEntry::Kind kind, const QString &name, const QString &target) {
    ComputerEntry entry;
    entry.kind = kind;
    entry.name = name;
    entry.target = target;
    return entry;
}

// Names in the model's visible order, ".." excluded (the computer root has none).
QStringList listedNames(const FileSystemModel &model) {
    QStringList names;
    for (int row = 0; row < model.rowCount(); ++row)
        names << model.fileInfoAt(row).name();
    return names;
}

QString typeAt(const FileSystemModel &model, int row) {
    return model.data(model.index(row, FileSystemModel::TypeColumn), Qt::DisplayRole).toString();
}

// Drives the model's asynchronous local scan to completion.
void waitForLoad(FileSystemModel &model) {
    QSignalSpy spy(&model, &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(5000);
}

} // namespace

TEST(ComputerViewTest, CatalogTest_EveryDriveIsNamedAndPointsSomewhereReal) {
    const QVector<ComputerEntry> drives = ComputerCatalog::drives();
    // A machine with no mounted volume at all is not a case worth asserting
    // about, but every entry that IS produced has to be usable.
    for (const ComputerEntry &drive : drives) {
        EXPECT_EQ(drive.kind, ComputerEntry::Kind::Drive);
        EXPECT_FALSE(drive.name.isEmpty());
        EXPECT_FALSE(drive.target.isEmpty());
        // The target is handed straight to navigateTo, so it must be a real
        // directory on this machine -- this is the assertion that would fail if
        // a pseudo filesystem ever slipped through the filter.
        EXPECT_TRUE(QDir(drive.target).exists())
            << drive.name.toStdString() << " -> " << drive.target.toStdString();
    }
}

TEST(ComputerViewTest, CatalogTest_OneDiskIsListedOnceEvenWhenMountedTwice) {
    // A bind mount puts the same device at a second mount point (WSL does it for
    // /mnt/wslg/distro; any fstab "bind" entry does it too). Listing both reads
    // as two disks that are really one.
    QSet<QString> devices;
    for (const ComputerEntry &drive : ComputerCatalog::drives()) {
        // Recovered from the name, which is "<label or mount> (<device>)" on
        // Linux; on Windows the parenthesised part is the drive letter, which is
        // just as unique per volume.
        const int open = drive.name.lastIndexOf(QLatin1Char('('));
        if (open < 0)
            continue;
        const QString device = drive.name.mid(open);
        EXPECT_FALSE(devices.contains(device))
            << "listed twice: " << device.toStdString() << " (" << drive.target.toStdString() << ")";
        devices.insert(device);
    }
}

TEST(ComputerViewTest, CatalogTest_DrivesExcludePseudoFilesystems) {
    for (const ComputerEntry &drive : ComputerCatalog::drives()) {
        // /proc, /sys, /run and the snap loopbacks under /snap are the ones that
        // flood the list if the filter regresses.
        EXPECT_FALSE(drive.target.startsWith(QStringLiteral("/proc")));
        EXPECT_FALSE(drive.target.startsWith(QStringLiteral("/sys")));
        EXPECT_FALSE(drive.target.startsWith(QStringLiteral("/snap")));
    }
}

TEST(ComputerViewTest, CatalogTest_UserFoldersAreExistingDirectoriesWithoutDuplicates) {
    const QVector<ComputerEntry> folders = ComputerCatalog::userFolders();
    QSet<QString> targets;
    for (const ComputerEntry &folder : folders) {
        EXPECT_EQ(folder.kind, ComputerEntry::Kind::UserFolder);
        EXPECT_FALSE(folder.name.isEmpty());
        EXPECT_TRUE(QDir(folder.target).exists()) << folder.target.toStdString();
        // Some setups point several standard locations at $HOME; showing "My
        // Documents" and "My Downloads" as the same folder twice is noise.
        EXPECT_FALSE(targets.contains(folder.target)) << folder.target.toStdString();
        targets.insert(folder.target);
    }
    // The desktop is the one location present on every platform this builds for,
    // so an empty result means the enumeration itself broke.
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!desktop.isEmpty() && QDir(desktop).exists())
        EXPECT_FALSE(folders.isEmpty());
}

TEST(ComputerViewTest, ProviderTest_ListsOneRowPerEntryAndMapsItBack) {
    ComputerProvider provider;
    provider.setEntries({
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("Windows (C:)"), QStringLiteral("C:/")),
        makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("Home NAS"),
                  QStringLiteral("uuid-1")),
    });

    const QVector<FileInfo> rows = provider.list(ComputerProvider::rootPath(), false);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).name(), QStringLiteral("Windows (C:)"));
    // Every row is a directory so the views treat it as navigable and never
    // offer to preview or edit one.
    EXPECT_TRUE(rows.at(0).isDir());

    // The row's path maps back to the entry it stands for; that mapping is what
    // lets an activated row be dispatched on its kind.
    const ComputerEntry back = provider.entryFor(rows.at(1).path());
    EXPECT_EQ(back.kind, ComputerEntry::Kind::SavedServer);
    EXPECT_EQ(back.target, QStringLiteral("uuid-1"));
}

TEST(ComputerViewTest, ProviderTest_PathsAreNeverMistakenForLocalFilesystemPaths) {
    ComputerProvider provider;
    provider.setEntries({makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("NAS"),
                                   QStringLiteral("uuid-1"))});
    const QVector<FileInfo> rows = provider.list(ComputerProvider::rootPath(), false);
    ASSERT_EQ(rows.size(), 1);

    // The whole point of the guard on FileProvider: code that is about to hand a
    // path to QFile asks this first, and a synthetic backend must say no.
    EXPECT_FALSE(provider.isLocalFilesystem());
    // And the path must not accidentally BE openable, so a caller that skips the
    // guard fails loudly instead of touching a same-named local file.
    EXPECT_FALSE(QFile::exists(rows.at(0).path()));
    EXPECT_TRUE(rows.at(0).path().startsWith(ComputerProvider::rootPath()));
}

TEST(ComputerViewTest, ProviderTest_RootHasNoParentSoTheListingGetsNoDotDotRow) {
    ComputerProvider provider;
    EXPECT_TRUE(provider.parentPath(ComputerProvider::rootPath()).isEmpty());
    EXPECT_TRUE(provider.isDir(ComputerProvider::rootPath()));
    // Nothing lists below a row: activating one navigates elsewhere entirely.
    EXPECT_TRUE(provider.list(QStringLiteral("computer://drive/C:/"), false).isEmpty());
}

TEST(ComputerViewTest, ProviderTest_RenameIsRefusedRatherThanReportedUnsupported) {
    ComputerProvider provider;
    QString newPath;
    // Unsupported means "ask another backend"; there is no other backend that
    // could rename a drive, so conflating the two would make the caller retry.
    EXPECT_EQ(provider.rename(QStringLiteral("computer://drive/C:/"), QStringLiteral("D"),
                              &newPath),
              FileProvider::RenameResult::Failed);
}

TEST(ComputerViewTest, ViewModelTest_TypeColumnNamesTheKindOfPlaceEachRowIs) {
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("Windows (C:)"), QStringLiteral("C:/")),
        makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("Home NAS"),
                  QStringLiteral("uuid-1")),
    });

    FileSystemModel model;
    model.setProvider(provider);
    model.setRootPath(ComputerProvider::rootPath());
    waitForLoad(model);

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(typeAt(model, 0), QStringLiteral("Drive"));
    // "Folder" -- what typeCategory() answers for any directory -- would be
    // wrong for a server, which is the reason the backend gets to name the type.
    EXPECT_EQ(typeAt(model, 1), QStringLiteral("Server"));
}

TEST(ComputerViewTest, ViewModelTest_SectionsHoldTheirOrderThroughEverySort) {
    auto provider = std::make_shared<ComputerProvider>();
    // Deliberately named so that alphabetical order would interleave the
    // sections: "aaa server" sorts before "zzz drive" on name.
    provider->setEntries({
        makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("aaa server"),
                  QStringLiteral("uuid-1")),
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("zzz drive"), QStringLiteral("C:/")),
        makeEntry(ComputerEntry::Kind::UserFolder, QStringLiteral("mmm folder"),
                  QStringLiteral("C:/Users")),
    });

    FileSystemModel model;
    model.setProvider(provider);
    model.setRootPath(ComputerProvider::rootPath());
    waitForLoad(model);

    const QStringList ascending = listedNames(model);
    ASSERT_EQ(ascending.size(), 3);
    EXPECT_EQ(ascending,
              (QStringList{QStringLiteral("zzz drive"), QStringLiteral("mmm folder"),
                           QStringLiteral("aaa server")}));

    // Reversing the sort reverses rows WITHIN a section, never the sections
    // themselves -- a network host must not surface above the drives.
    model.sort(FileSystemModel::NameColumn, Qt::DescendingOrder);
    const QStringList descending = listedNames(model);
    EXPECT_EQ(descending,
              (QStringList{QStringLiteral("zzz drive"), QStringLiteral("mmm folder"),
                           QStringLiteral("aaa server")}));
}

TEST(ComputerViewTest, ViewModelTest_CreatedColumnShowsWhenABookmarkWasSaved) {
    const QDateTime saved = QDateTime::fromString(QStringLiteral("2026-01-02 03:04"),
                                                  QStringLiteral("yyyy-MM-dd HH:mm"));
    ComputerEntry server =
        makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("NAS"), QStringLiteral("uuid"));
    server.created = saved;
    ComputerEntry drive =
        makeEntry(ComputerEntry::Kind::Drive, QStringLiteral("C"), QStringLiteral("C:/"));

    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({drive, server});

    FileSystemModel model;
    model.setProvider(provider);
    model.setRootPath(ComputerProvider::rootPath());
    waitForLoad(model);

    ASSERT_EQ(model.rowCount(), 2);
    const auto created = [&](int row) {
        return model.data(model.index(row, FileSystemModel::CreatedColumn), Qt::DisplayRole)
            .toString();
    };
    // A drive has no creation date anyone could state, so the cell stays blank
    // rather than showing the epoch or today.
    EXPECT_TRUE(created(0).isEmpty());
    EXPECT_EQ(created(1), QStringLiteral("2026-01-02 03:04"));
}

TEST(ComputerViewTest, ViewModelTest_SwappingBackToALocalProviderDropsTheVirtualBehaviour) {
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({makeEntry(ComputerEntry::Kind::SavedServer, QStringLiteral("NAS"),
                                    QStringLiteral("uuid"))});
    FileSystemModel model;
    model.setProvider(provider);
    model.setRootPath(ComputerProvider::rootPath());
    waitForLoad(model);
    ASSERT_EQ(model.rowCount(), 1);
    ASSERT_EQ(typeAt(model, 0), QStringLiteral("Server"));

    // Back to the local backend: a real directory must be described by the
    // ordinary type rules again, not by whatever the last provider claimed.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("sub")));
    model.setProvider(nullptr);
    model.setRootPath(dir.path());
    waitForLoad(model);

    ASSERT_GE(model.rowCount(), 1);
    const int row = model.isParentEntry(0) ? 1 : 0;
    ASSERT_LT(row, model.rowCount());
    EXPECT_EQ(typeAt(model, row), QStringLiteral("Folder"));
}

// QStorageInfo enumerates mounts in whatever order the OS reports them, which
// on Windows commonly puts D: before C:. Drives are listed in the order people
// expect to find them instead.
TEST(ComputerViewTest, CatalogOrderTest_DrivesAreListedByMountRootNotByLabel) {
    const QVector<ComputerEntry> drives = ComputerCatalog::drives();
    if (drives.size() < 2)
        GTEST_SKIP() << "only " << drives.size() << " drive(s) on this machine";

    QStringList roots;
    for (const ComputerEntry &drive : drives)
        roots << drive.target;

    QStringList sorted = roots;
    std::sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    EXPECT_EQ(roots, sorted) << "drives came back as " << roots.join(QLatin1String(", ")).toStdString();

#ifdef Q_OS_WIN
    // The identity is the drive letter, and it is what the order has to follow:
    // sorting by the display name would put a disk labelled "ntfs" ahead of the
    // system disk.
    for (const ComputerEntry &drive : drives) {
        SCOPED_TRACE(drive.target.toStdString());
        EXPECT_TRUE(drive.target.size() >= 2 && drive.target.at(1) == QLatin1Char(':'))
            << "a Windows drive row should be a volume (a drive letter), not a physical disk";
    }
#endif
}
