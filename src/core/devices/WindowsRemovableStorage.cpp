#include "WindowsRemovableStorage.h"

#define NOMINMAX
#include <windows.h>

bool WindowsRemovableStorage::shouldExposeAsRemovable(unsigned driveType, bool deviceHotplug,
                                                       bool mediaRemovable) {
    return driveType == DRIVE_REMOVABLE || deviceHotplug || mediaRemovable;
}
