#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStorageInfo>
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
    // A bind mount puts the same device at a second mount point (an ostree root
    // does it for /home, /var and /root off one partition; WSL does it for
    // /mnt/wslg/distro; any fstab "bind" entry does it too). Listing them all
    // reads as several disks that are really one.
    //
    // Asked of the mount point rather than parsed out of the display name: the
    // name is a user-facing string that has already changed shape once, and the
    // device behind a mount point is the thing actually being asserted about.
    QSet<QString> devices;
    for (const ComputerEntry &drive : ComputerCatalog::drives()) {
        const QString device = QString::fromLocal8Bit(QStorageInfo(drive.target).device());
        if (device.isEmpty())
            continue;
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

#ifndef Q_OS_WIN
TEST(ComputerViewTest, CatalogTest_SystemMountPointsAreHiddenButTheRootIsNot) {
    // These are real block-device filesystems, so the pseudo-filesystem filter
    // cannot remove them -- but nobody browses to /boot from a file manager,
    // and an ostree layout mounts most of this list off one partition.
    const QStringList hidden = {
        QStringLiteral("/boot"),       QStringLiteral("/efi"),  QStringLiteral("/sysroot"),
        QStringLiteral("/ostree"),     QStringLiteral("/var"),  QStringLiteral("/persistent"),
        QStringLiteral("/persistent/ostree"), QStringLiteral("/root"), QStringLiteral("/usr"),
    };
    bool sawRoot = false;
    for (const ComputerEntry &drive : ComputerCatalog::drives()) {
        EXPECT_FALSE(hidden.contains(drive.target)) << drive.target.toStdString();
        if (drive.target == QStringLiteral("/"))
            sawRoot = true;
    }
    // The prefix matching must never swallow "/" itself: it is the prefix of
    // every path in the list above, and hiding it would leave a machine with no
    // system disk at all.
    EXPECT_TRUE(sawRoot) << "the root filesystem is missing from the drive list";
}

TEST(ComputerViewTest, CatalogTest_DrivesAreNamedByMountPointNotByDeviceNode) {
    for (const ComputerEntry &drive : ComputerCatalog::drives()) {
        // "/dev/nvme0n1p5" identifies the volume precisely and means nothing to
        // anyone who is not in a terminal -- and neither does the filesystem
        // label an installer wrote ("Roota", "_dde_data"). The mount point is
        // the whole name.
        EXPECT_EQ(drive.name, drive.target) << drive.name.toStdString();
    }
}

TEST(ComputerViewTest, CatalogTest_ShowingSystemVolumesRestoresEveryMountPoint) {
    const QVector<ComputerEntry> plain = ComputerCatalog::drives(false);
    const QVector<ComputerEntry> all = ComputerCatalog::drives(true);
    // Strictly a superset: the flag only ever stops rows being dropped.
    EXPECT_GE(all.size(), plain.size());

    QSet<QString> allTargets;
    for (const ComputerEntry &drive : all)
        allTargets.insert(drive.target);
    for (const ComputerEntry &drive : plain)
        EXPECT_TRUE(allTargets.contains(drive.target)) << drive.target.toStdString();

    if (all.size() < 2)
        GTEST_SKIP() << "only " << all.size() << " mounted volume(s) on this machine";
    // And where a disk really is mounted more than once, the two modes have to
    // differ -- otherwise the deduplication above passed only because this
    // machine had no bind mounts to collapse.
    QSet<QString> devices;
    bool repeatedDevice = false;
    for (const ComputerEntry &drive : all) {
        const QString device = QString::fromLocal8Bit(QStorageInfo(drive.target).device());
        if (device.isEmpty())
            continue;
        if (devices.contains(device))
            repeatedDevice = true;
        devices.insert(device);
    }
    if (repeatedDevice)
        EXPECT_LT(plain.size(), all.size());
}
#endif

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

TEST(ComputerViewTest, CatalogTest_ReceivedFilesIsListedLastOnceItExists) {
    // Both halves of the feature agree on this one directory: the share server
    // publishes it and the computer view lists it, so a wrong path here means
    // files arrive somewhere nobody looks.
    const QString received = ComputerCatalog::receivedFilesPath();
    EXPECT_EQ(received, QDir::homePath() + QStringLiteral("/ReceivedFiles"));

    const QVector<ComputerEntry> folders = ComputerCatalog::userFolders();
    int index = -1;
    for (int i = 0; i < folders.size(); ++i) {
        if (folders.at(i).target == received)
            index = i;
    }
    if (QFileInfo(received).isDir()) {
        ASSERT_GE(index, 0) << "existing received-files folder was not listed";
        EXPECT_EQ(index, folders.size() - 1);
    } else {
        // A row pointing at a directory that is not there would navigate to
        // nothing, so nothing is offered until the first file arrives.
        EXPECT_EQ(index, -1);
    }
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

ComputerEntry makeAccountDevice(const QString &name, const QString &target, bool online) {
    ComputerEntry entry;
    entry.kind = ComputerEntry::Kind::AccountDevice;
    entry.name = name;
    entry.target = target;
    entry.online = online;
    return entry;
}

TEST(ComputerViewTest, ProviderTest_AccountDeviceMapsBackAndReportsItsOnlineState) {
    ComputerProvider provider;
    provider.setEntries({
        makeAccountDevice(QStringLiteral("work laptop"), QStringLiteral("dev-1"), true),
        makeAccountDevice(QStringLiteral("home tower"), QStringLiteral("dev-2"), false),
    });

    const QVector<FileInfo> rows = provider.list(ComputerProvider::rootPath(), false);
    ASSERT_EQ(rows.size(), 2);

    const ComputerEntry online = provider.entryFor(rows.at(0).path());
    EXPECT_EQ(online.kind, ComputerEntry::Kind::AccountDevice);
    EXPECT_EQ(online.target, QStringLiteral("dev-1"));
    EXPECT_TRUE(online.online);
    EXPECT_TRUE(provider.entryEnabled(rows.at(0).path()));
    EXPECT_EQ(provider.entryTypeLabel(rows.at(0).path()), QStringLiteral("Device"));

    // An offline device is still listed (so the section reads as "my devices"
    // even when nothing is reachable) but answers not-enabled, which is what
    // greys and deactivates it.
    const ComputerEntry offline = provider.entryFor(rows.at(1).path());
    EXPECT_EQ(offline.target, QStringLiteral("dev-2"));
    EXPECT_FALSE(offline.online);
    EXPECT_FALSE(provider.entryEnabled(rows.at(1).path()));
}

TEST(ComputerViewTest, ViewModelTest_OfflineAccountDeviceRemainsSelectable) {
    auto provider = std::make_shared<ComputerProvider>();
    provider->setEntries({
        makeAccountDevice(QStringLiteral("work laptop"), QStringLiteral("dev-1"), true),
        makeAccountDevice(QStringLiteral("home tower"), QStringLiteral("dev-2"), false),
    });

    FileSystemModel model;
    model.setProvider(provider);
    model.setRootPath(ComputerProvider::rootPath());
    waitForLoad(model);

    ASSERT_EQ(model.rowCount(), 2);

    // Rows sort by name within their section, so find each by name rather than
    // by insertion order.
    int onlineRow = -1, offlineRow = -1;
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.fileInfoAt(row).name() == QStringLiteral("work laptop"))
            onlineRow = row;
        if (model.fileInfoAt(row).name() == QStringLiteral("home tower"))
            offlineRow = row;
    }
    ASSERT_NE(onlineRow, -1);
    ASSERT_NE(offlineRow, -1);

    const QModelIndex online = model.index(onlineRow, FileSystemModel::NameColumn);
    const QModelIndex offline = model.index(offlineRow, FileSystemModel::NameColumn);
    EXPECT_TRUE(model.flags(online) & Qt::ItemIsEnabled);
    // Offline peers remain keyboard/mouse selectable; activation reports their
    // state instead of silently ignoring the user.
    EXPECT_TRUE(model.flags(offline) & Qt::ItemIsEnabled);
    EXPECT_TRUE(model.flags(offline) & Qt::ItemIsSelectable);
}
