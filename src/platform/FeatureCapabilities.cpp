#include "FeatureCapabilities.h"

FeatureCapabilities FeatureCapabilities::build() {
    return {
        FILECOMMANDER_HAS_NETWORK != 0,
        FILECOMMANDER_HAS_PREVIEW_PDF != 0,
        FILECOMMANDER_HAS_PREVIEW_MEDIA != 0,
        FILECOMMANDER_HAS_PREVIEW_OFFICE != 0,
        FILECOMMANDER_HAS_LINUX_INTEGRATION != 0,
        FILECOMMANDER_HAS_LINUX_INTEGRATION != 0,
    };
}
