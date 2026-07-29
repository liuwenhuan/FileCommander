#include "FeatureCapabilities.h"

namespace {
#if defined(Q_OS_WIN)
constexpr bool kHasLocalDeviceFeatures = true;
#else
constexpr bool kHasLocalDeviceFeatures = FILECOMMANDER_HAS_LINUX_INTEGRATION != 0;
#endif
} // namespace

FeatureCapabilities FeatureCapabilities::build() {
    return {
        FILECOMMANDER_HAS_NETWORK != 0,
        FILECOMMANDER_HAS_PREVIEW_PDF != 0,
        FILECOMMANDER_HAS_PREVIEW_MEDIA != 0,
        FILECOMMANDER_HAS_PREVIEW_OFFICE != 0,
        kHasLocalDeviceFeatures,
        kHasLocalDeviceFeatures,
    };
}
