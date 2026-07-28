#pragma once

struct FeatureCapabilities {
    bool network = false;
    bool pdfPreview = false;
    bool mediaPreview = false;
    bool officePreview = false;
    bool secureWipe = false;
    bool selfUpdate = false;

    static FeatureCapabilities build();
};
