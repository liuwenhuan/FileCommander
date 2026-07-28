#include <gtest/gtest.h>

#include "FeatureCapabilities.h"

TEST(FeatureCapabilities, MirrorsCompileTimeFeatureSelection) {
    const auto caps = FeatureCapabilities::build();
    EXPECT_EQ(caps.network, FILECOMMANDER_HAS_NETWORK != 0);
    EXPECT_EQ(caps.pdfPreview, FILECOMMANDER_HAS_PREVIEW_PDF != 0);
    EXPECT_EQ(caps.mediaPreview, FILECOMMANDER_HAS_PREVIEW_MEDIA != 0);
    EXPECT_EQ(caps.officePreview, FILECOMMANDER_HAS_PREVIEW_OFFICE != 0);
    EXPECT_EQ(caps.secureWipe, FILECOMMANDER_HAS_LINUX_INTEGRATION != 0);
}
