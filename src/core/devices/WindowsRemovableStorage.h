#pragma once

namespace WindowsRemovableStorage {

// Windows calls many USB storage devices DRIVE_FIXED. The physical disk's
// hot-plug/media flags distinguish those from an internal SSD.
bool shouldExposeAsRemovable(unsigned driveType, bool deviceHotplug, bool mediaRemovable);

} // namespace WindowsRemovableStorage
