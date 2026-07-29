#include <gtest/gtest.h>

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

} // namespace
