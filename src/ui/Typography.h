#pragma once

#include <QFont>

class Settings;
class QWidget;

// Applies the user-selected family everywhere while preserving the separate
// menu and file-list point sizes.
class Typography {
public:
    static void initializeSystemFont();
    static QFont systemFont();
    static QFont chromeFont(const Settings &settings);
    static void applyApplicationFont(const Settings &settings);
    static void applyChromeFont(QWidget *widget, const Settings &settings);
};
