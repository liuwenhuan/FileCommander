#include <gtest/gtest.h>

#include <QDir>
#include <QSet>

#include "tree/TreeRootBuilder.h"

namespace {

LocalVolume volume(const QString &name, const QString &mount) {
    LocalVolume v;
    v.name = name;
    v.mountPoint = mount;
    return v;
}

RemovableDevice usbStick(const QString &id, const QString &name, const QString &mount) {
    RemovableDevice d;
    d.id = id;
    d.name = name;
    d.mountPoint = mount;
    d.iconName = QStringLiteral("dev-usb");
    d.isMounted = !mount.isEmpty();
    return d;
}

NetworkTreeEntry smbEntry(const QString &id, const QString &label) {
    NetworkTreeEntry e;
    e.connectionId = id;
    e.label = label;
    e.scheme = QStringLiteral("smb");
    e.basePath = QStringLiteral("/");
    return e;
}

} // namespace

// The overwhelmingly common setup -- one disk, nothing plugged in, nothing
// connected -- must keep producing the plain whole-filesystem tree. A device
// header row appearing here would be a visible regression for nearly every user,
// so this is the single most important rule in the builder.
TEST(TreeRootBuilderTest, SingleDiskNoExternalsCollapsesToPlainFilesystemRoot) {
    const auto roots = TreeRootBuilder::build({volume("System", "/")}, {}, {});

    ASSERT_EQ(roots.size(), 1);
    EXPECT_EQ(roots.first().kind, TreeRoot::LocalFilesystem);
    EXPECT_EQ(roots.first().basePath, QDir::rootPath());
    EXPECT_TRUE(roots.first().activatable);
}

// No volumes at all (an environment where QStorageInfo reports nothing usable)
// must still yield a browsable root rather than an empty tree.
TEST(TreeRootBuilderTest, NoVolumesStillYieldsFilesystemRoot) {
    const auto roots = TreeRootBuilder::build({}, {}, {});

    ASSERT_EQ(roots.size(), 1);
    EXPECT_EQ(roots.first().kind, TreeRoot::LocalFilesystem);
    EXPECT_EQ(roots.first().basePath, QDir::rootPath());
}

// A second disk is exactly the case the collapse rule must NOT apply to.
TEST(TreeRootBuilderTest, TwoDisksProduceOneVolumeRootEach) {
    const auto roots =
        TreeRootBuilder::build({volume("System", "/"), volume("Data", "/mnt/data")}, {}, {});

    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots.at(0).kind, TreeRoot::LocalVolume);
    EXPECT_EQ(roots.at(0).basePath, QString("/"));
    EXPECT_EQ(roots.at(1).kind, TreeRoot::LocalVolume);
    EXPECT_EQ(roots.at(1).basePath, QString("/mnt/data"));
}

// Plugging a USB stick into a single-disk machine breaks the collapse: the disk
// and the stick each become their own root.
TEST(TreeRootBuilderTest, RemovableDeviceBreaksTheSingleDiskCollapse) {
    const auto roots = TreeRootBuilder::build({volume("System", "/")},
                                              {usbStick("/dev/sdb1", "MyStick", "/media/stick")},
                                              {});

    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots.at(0).kind, TreeRoot::LocalVolume);
    EXPECT_EQ(roots.at(1).kind, TreeRoot::Removable);
    EXPECT_EQ(roots.at(1).label, QString("MyStick"));
    EXPECT_EQ(roots.at(1).basePath, QString("/media/stick"));
    EXPECT_EQ(roots.at(1).iconName, QString("dev-usb"));
    EXPECT_EQ(roots.at(1).deviceId, QString("/dev/sdb1"));
}

// An unmounted device has no path to root a tree at, so it must not appear --
// even though it is a real, present device (the external-connection panel is
// where it can be mounted from).
TEST(TreeRootBuilderTest, UnmountedRemovableDeviceIsNotARoot) {
    const auto roots =
        TreeRootBuilder::build({volume("System", "/")}, {usbStick("/dev/sdb1", "MyStick", "")}, {});

    ASSERT_EQ(roots.size(), 1);
    EXPECT_EQ(roots.first().kind, TreeRoot::LocalFilesystem); // collapse still applies
}

// A network connection alone also breaks the collapse, and carries its
// protocol icon.
TEST(TreeRootBuilderTest, NetworkConnectionBecomesItsOwnRootWithProtocolIcon) {
    const auto roots = TreeRootBuilder::build({volume("System", "/")}, {},
                                              {smbEntry("smb://deepin@192.168.1.2",
                                                        "deepin@192.168.1.2")});

    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots.at(1).kind, TreeRoot::Network);
    EXPECT_EQ(roots.at(1).iconName, QString("dev-smb"));
    EXPECT_EQ(roots.at(1).connectionId, QString("smb://deepin@192.168.1.2"));
    EXPECT_TRUE(roots.at(1).activatable);
}

// Each protocol gets its own icon so connection types are distinguishable at a
// glance; all of these exist under resources/icons.
TEST(TreeRootBuilderTest, EachProtocolMapsToItsOwnIcon) {
    for (const QString &scheme : {QStringLiteral("smb"), QStringLiteral("sftp"),
                                  QStringLiteral("ftp"), QStringLiteral("webdav")}) {
        NetworkTreeEntry entry = smbEntry("id-" + scheme, "host");
        entry.scheme = scheme;
        const auto roots = TreeRootBuilder::build({volume("System", "/")}, {}, {entry});
        ASSERT_EQ(roots.size(), 2) << scheme.toStdString();
        EXPECT_EQ(roots.at(1).iconName, QStringLiteral("dev-") + scheme);
    }
}

// A connection owned by the other panel is shown (it is useful to see what is
// connected) but must not be activatable: adopting another panel's session would
// break per-tab connection ownership.
TEST(TreeRootBuilderTest, OtherPanelsConnectionIsShownButNotActivatable) {
    NetworkTreeEntry entry = smbEntry("smb://deepin@192.168.1.2", "deepin@192.168.1.2");
    entry.ownedByThisPanel = false;
    const auto roots = TreeRootBuilder::build({volume("System", "/")}, {}, {entry});

    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots.at(1).kind, TreeRoot::Network);
    EXPECT_FALSE(roots.at(1).activatable);
}

// Ordering is deliberate and stable: disks, then removable volumes, then
// connections. A tree that reshuffles its roots on every hot-plug would be
// unusable.
TEST(TreeRootBuilderTest, RootOrderIsDisksThenRemovableThenNetwork) {
    const auto roots = TreeRootBuilder::build(
        {volume("System", "/"), volume("Data", "/mnt/data")},
        {usbStick("/dev/sdb1", "Stick", "/media/stick")},
        {smbEntry("smb://deepin@host", "deepin@host")});

    ASSERT_EQ(roots.size(), 4);
    EXPECT_EQ(roots.at(0).kind, TreeRoot::LocalVolume);
    EXPECT_EQ(roots.at(1).kind, TreeRoot::LocalVolume);
    EXPECT_EQ(roots.at(2).kind, TreeRoot::Removable);
    EXPECT_EQ(roots.at(3).kind, TreeRoot::Network);
}

// A device the monitor could not classify still needs some icon rather than a
// blank row.
TEST(TreeRootBuilderTest, UnclassifiedRemovableDeviceFallsBackToGenericIcon) {
    RemovableDevice device = usbStick("/dev/sdc1", "Unknown", "/media/x");
    device.iconName.clear();
    const auto roots = TreeRootBuilder::build({volume("System", "/")}, {device}, {});

    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots.at(1).iconName, QString("dev-drive"));
}

// The candidate list drives "start from the topmost VISIBLE directory": try the
// provider root first, then descend one segment at a time towards where the tab
// actually is, so the root settles at the highest listable level.
TEST(TreeRootBuilderTest, NetworkBaseCandidatesGoTopmostFirst) {
    const QStringList candidates = TreeRootBuilder::networkBaseCandidates("/share/sub/deep");

    ASSERT_EQ(candidates.size(), 4);
    EXPECT_EQ(candidates.at(0), QString("/"));
    EXPECT_EQ(candidates.at(1), QString("/share"));
    EXPECT_EQ(candidates.at(2), QString("/share/sub"));
    EXPECT_EQ(candidates.at(3), QString("/share/sub/deep"));
}

TEST(TreeRootBuilderTest, NetworkBaseCandidatesForRootIsJustRoot) {
    const QStringList candidates = TreeRootBuilder::networkBaseCandidates("/");

    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates.first(), QString("/"));
}

// Enumeration must never surface kernel/pseudo filesystems as browsable disks.
// This runs against the real machine, so it asserts on properties rather than
// an exact set. Note the prefixes are matched as whole path segments: "/sysroot"
// is a real ext4 mount on an ostree system, unlike "/sys".
TEST(TreeRootBuilderTest, EnumerateLocalVolumesExcludesPseudoFilesystems) {
    const auto volumes = TreeRootBuilder::enumerateLocalVolumes({});

    for (const LocalVolume &v : volumes) {
        EXPECT_FALSE(v.mountPoint.isEmpty());
        EXPECT_FALSE(v.name.isEmpty());
        for (const QString &pseudo : {QStringLiteral("/proc"), QStringLiteral("/sys"),
                                      QStringLiteral("/dev"), QStringLiteral("/run")}) {
            EXPECT_NE(v.mountPoint, pseudo);
            EXPECT_FALSE(v.mountPoint.startsWith(pseudo + QLatin1Char('/')))
                << v.mountPoint.toStdString();
        }
    }
}

// The requirement is per DISK, not per mount. A normal install puts /, /boot,
// /home and friends on partitions of ONE drive, and an ostree system adds
// several more. Emitting a root for each would both duplicate subtrees that are
// already reachable from "/" and -- much worse -- stop an ordinary single-disk
// machine from ever taking the plain-filesystem path.
TEST(TreeRootBuilderTest, EnumerateLocalVolumesYieldsOneEntryPerPhysicalDrive) {
    const auto volumes = TreeRootBuilder::enumerateLocalVolumes({});
    if (volumes.isEmpty())
        GTEST_SKIP() << "no local volumes on this machine";

    QSet<QString> mounts;
    for (const LocalVolume &v : volumes) {
        EXPECT_FALSE(mounts.contains(v.mountPoint)) << "duplicate mount " << v.mountPoint.toStdString();
        mounts.insert(v.mountPoint);
    }
    // The real check: this machine's many device-backed mounts (/, /boot,
    // /home, /var, /sysroot, ... all on one NVMe drive) must collapse to far
    // fewer entries than there are mounts -- one per drive.
    EXPECT_LE(volumes.size(), 4) << "expected roughly one root per physical drive";
}

// The machine this runs on has a root filesystem, so enumeration must find it
// and it must be the entry for its drive.
TEST(TreeRootBuilderTest, EnumerateLocalVolumesIncludesTheRootFilesystem) {
    const auto volumes = TreeRootBuilder::enumerateLocalVolumes({});

    bool hasRoot = false;
    for (const LocalVolume &v : volumes)
        if (v.mountPoint == QDir::rootPath())
            hasRoot = true;
    EXPECT_TRUE(hasRoot);
}

// A mount already reported by RemovableDeviceMonitor must not also appear as a
// fixed disk, or the same volume would get two roots.
//
// Only the exclusion itself is asserted, not the resulting count: excluding a
// drive's shallowest mount lets a deeper mount on that same drive take the slot
// (on this ostree machine, excluding "/" promotes "/boot"), so the total need
// not drop by one.
TEST(TreeRootBuilderTest, EnumerateLocalVolumesSkipsKnownRemovableMounts) {
    const auto all = TreeRootBuilder::enumerateLocalVolumes({});
    if (all.isEmpty())
        GTEST_SKIP() << "no local volumes on this machine";

    const QString excluded = all.first().mountPoint;
    const auto filtered = TreeRootBuilder::enumerateLocalVolumes({excluded});

    for (const LocalVolume &v : filtered)
        EXPECT_NE(v.mountPoint, excluded);
}
