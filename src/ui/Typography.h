#pragma once

#include <QFont>

class Settings;
class QWidget;
class QMenu;

// Applies the user-selected family everywhere while preserving the separate
// menu and file-list point sizes.
class Typography {
public:
    static void initializeSystemFont();
    static QFont systemFont();
    static QFont chromeFont(const Settings &settings);
    static void applyApplicationFont(const Settings &settings);
    static void applyApplicationFont(const QFont &font);
    static void applyChromeFont(QWidget *widget, const Settings &settings);
    static void applyChromeFont(QWidget *widget, const QFont &font);

    // Same as applyChromeFont, but also assigns the font to every descendant
    // explicitly instead of relying on Qt's inheritance to carry it down.
    //
    // Inheritance is not dependable here for two compounding reasons. First,
    // applyApplicationFont() runs before these calls, so by the time a container
    // is asked to change, its own (unset, therefore app-derived) font already
    // equals the new font -- setFont() is skipped as a no-op and no FontChange is
    // ever delivered to its children. Second, the themes install an application
    // stylesheet, and QStyleSheetStyle resolves a font onto widgets at polish
    // time, which detaches them from later propagation anyway.
    //
    // ONLY for containers that hold pure chrome (title bar, function-key bar,
    // command bar). Never for MainWindow or FilePanel: those contain the file
    // views, which carry the separate, independently-configured list font that
    // this must not overwrite.
    static void applyChromeFontToTree(QWidget *root, const QFont &font);

    // Forces every embedded font-size-stepper widget (see applyChromeFont's
    // QWidgetAction handling) under menu to match menu's own current font, right
    // now -- synchronously, not via MenuChromeSynchronizer's deferred
    // QTimer::singleShot(0, ...) re-sync off a FontChange event. Call this after
    // setting menu's own font directly (e.g. via applyChromeFont(menu, font)) when
    // the menu may currently be open and the embedded content must not wait for
    // the next event-loop turn to catch up.
    static void refreshOpenMenuChrome(QMenu *menu);
};
