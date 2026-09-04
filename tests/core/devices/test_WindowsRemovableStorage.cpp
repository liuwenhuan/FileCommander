#include <gtest/gtest.h>

#include <QStorageInfo>

#include "devices/RemovableDeviceMonitor.h"
#include "devices/WindowsRemovableStorage.h"

namespace {

constexpr unsigned kDriveRemovable = 2;
constexpr unsigned kDriveFixed = 3;

TEST(WindowsRemovableStorageTest, KeepsInternalFixedSsdOutOfRemovableDevices) {
    EXPECT_FALSE(WindowsRemovableStorage::shouldExposeAsRemovable(
        kDriveFixed, /*deviceHotplug=*/false, /*mediaRemovable=*/false));
}

TEST(WindowsRemovableStorageTest, IncludesUsbStorageEvenWhenWindowsCallsItFixed) {
    EXPECT_TRUE(WindowsRemovableStorage::shouldExposeAsRemovable(
        kDriveFixed, /*deviceHotplug=*/true, /*mediaRemovable=*/false));
}

TEST(WindowsRemovableStorageTest, IncludesConventionalRemovableDrives) {
    EXPECT_TRUE(WindowsRemovableStorage::shouldExposeAsRemovable(
        kDriveRemovable, /*deviceHotplug=*/false, /*mediaRemovable=*/false));
}

TEST(RemovableDeviceTest, UnknownStorageMetricsUseExplicitUnavailableSentinels) {
    const RemovableDevice device;

    EXPECT_EQ(device.bytesTotal, -1);
    EXPECT_EQ(device.bytesAvailable, -1);
}

TEST(RemovableDeviceMonitorTest, ReportsQStorageInfoMetricsForMountedRemovableVolumes) {
    RemovableDeviceMonitor monitor;
    const QVector<RemovableDevice> devices = monitor.devices();
    if (devices.isEmpty())
        GTEST_SKIP() << "No mounted removable volume is available for this environment";

    for (const RemovableDevice &device : devices) {
        ASSERT_TRUE(device.isMounted);
        ASSERT_FALSE(device.mountPoint.isEmpty());

        const QStorageInfo storage(device.mountPoint);
        ASSERT_TRUE(storage.isValid());
        ASSERT_TRUE(storage.isReady());
        EXPECT_EQ(device.bytesTotal, storage.bytesTotal() >= 0 ? storage.bytesTotal() : -1);
        EXPECT_EQ(device.bytesAvailable,
                  storage.bytesAvailable() >= 0 ? storage.bytesAvailable() : -1);
    }
}

} // namespace
