#include "Typography.h"

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QWidgetAction>

#include "Settings.h"

namespace {

constexpr auto kMenuChromeSurfaceProperty = "menuChromeSurface";
constexpr auto kMenuChromeSyncInstalledProperty = "menuChromeSyncInstalled";

void applyMenuFont(QWidget *widget, const QFont &font) {
    // Assigned unconditionally rather than only when it differs. A
    // QWidgetAction's default widget is detached from the menu whenever the menu
    // is not on screen, and a widget carrying no font of its own then re-resolves
    // against the APPLICATION font instead of the menu's. On Windows those are
    // two different families: the platform theme gives QMenu its own (measured:
    // Microsoft YaHei UI) while the application font is the general default
    // (SimSun). A row that merely inherited the right font while it was still
    // parented to the menu therefore came back in the wrong family and size --
    // which is exactly what made the two font-size rows render unlike every
    // entry beside them until the setting was touched once.
    widget->setFont(font);
    widget->updateGeometry();
}

void syncMenuChromeSurfaces(QMenu *menu) {
    if (!menu)
        return;

    // menu->font() is only the right anchor when the menu actually owns that
    // font. Qt hands QMenu a class-specific default on Windows -- measured as
    // Microsoft YaHei UI where the application font was SimSun -- and a class
    // default is NOT inherited by child widgets: they resolve against the
    // application font. Following menu->font() in that case is what made the
    // embedded rows render in a different family from the entries beside them.
    // WA_SetFont distinguishes the two cases exactly: set means somebody called
    // setFont() on this menu and its children really do inherit it.
    const QFont chrome =
        menu->testAttribute(Qt::WA_SetFont) ? menu->font() : QApplication::font();

    // Reached through the menu's own actions rather than findChildren(): a
    // QWidgetAction's default widget is only parented to the menu while the menu
    // is on screen, so a direct-children search misses exactly the rows this is
    // meant to fix whenever it runs at any other moment -- including right after
    // they are built.
    for (QAction *action : menu->actions()) {
        auto *widgetAction = qobject_cast<QWidgetAction *>(action);
        if (!widgetAction)
            continue;
        QWidget *row = widgetAction->defaultWidget();
        if (!row)
            continue;
        applyMenuFont(row, chrome);
        for (QWidget *descendant : row->findChildren<QWidget *>())
            applyMenuFont(descendant, chrome);
        if (widgetAction->font() != chrome) {
            // QAction::setFont emits changed(), which is the public signal QMenu
            // uses to invalidate its cached action geometry.
            widgetAction->setFont(chrome);
        }
    }
    menu->updateGeometry();
    menu->adjustSize();
}

class MenuChromeSynchronizer final : public QObject {
public:
    static MenuChromeSynchronizer &instance() {
        static MenuChromeSynchronizer synchronizer;
        return synchronizer;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        // Show and Polish matter as much as the font events: a QMenu resolves its
        // own font (on Windows the platform theme gives the class its own family,
        // distinct from the application font) only once it is polished, which
        // happens AFTER aboutToShow has built the embedded rows. Syncing on the
        // font events alone left those rows holding whatever the menu reported
        // beforehand -- the application family -- with nothing later to correct
        // it, since no font event follows.
        if (event->type() != QEvent::FontChange &&
            event->type() != QEvent::ApplicationFontChange &&
            event->type() != QEvent::StyleChange && event->type() != QEvent::Show &&
            event->type() != QEvent::Polish) {
            return false;
        }

        auto *menu = qobject_cast<QMenu *>(watched);
        QPointer<QMenu> guardedMenu(menu);
        QTimer::singleShot(0, menu, [guardedMenu] {
            if (guardedMenu)
                syncMenuChromeSurfaces(guardedMenu);
        });
        return false;
    }
};

QFont &storedSystemFont() {
    static QFont font;
    return font;
}

bool &systemFontInitialized() {
    static bool initialized = false;
    return initialized;
}

bool isInstalledFamily(const QString &family) {
    const QStringList families = QFontDatabase().families();
    for (const QString &installed : families) {
        if (installed.compare(family, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

} // namespace

void Typography::initializeSystemFont() {
    if (systemFontInitialized())
        return;
    storedSystemFont() = QApplication::font();
    systemFontInitialized() = true;
}

QFont Typography::systemFont() {
    initializeSystemFont();
    return storedSystemFont();
}

QFont Typography::chromeFont(const Settings &settings) {
    QFont font = systemFont();
    const QString family = settings.globalFontFamily();
    if (!family.isEmpty() && isInstalledFamily(family))
        font.setFamily(family);
    font.setPointSize(settings.menuFontSize());
    return font;
}

void Typography::applyApplicationFont(const Settings &settings) {
    applyApplicationFont(chromeFont(settings));
}

void Typography::applyApplicationFont(const QFont &font) {
    if (QApplication::font() != font)
        QApplication::setFont(font);
    // QMenu carries a class-specific default of its own, which the Windows
    // platform theme fills in (measured: Microsoft YaHei UI where the general
    // application font was SimSun). That default wins over the application font
    // for the menu itself but is NOT inherited by child widgets, so the entries
    // QMenu painted and the widgets embedded in it through QWidgetAction ended
    // up in two different families at the same point size. Overriding the class
    // default is what makes the two agree at the source, instead of chasing the
    // difference afterwards on every widget.
    if (QApplication::font("QMenu") != font)
        QApplication::setFont(font, "QMenu");
}

void Typography::applyChromeFont(QWidget *widget, const Settings &settings) {
    applyChromeFont(widget, chromeFont(settings));
}

void Typography::applyChromeFont(QWidget *widget, const QFont &font) {
    if (!widget)
        return;

    // QWidgetAction content is painted as a real child widget above QMenu.
    // Mark that surface so themes can leave it transparent and preserve the
    // menu's own background (notably the CRT scanline texture). Clear any
    // resolved font left by an earlier application/theme update as well: plain
    // actions inherit QMenu, and embedded rows must do the same to track live
    // menu-font changes and recalculate their action height.
    if (auto *menu = qobject_cast<QMenu *>(widget->parentWidget());
        menu && !qobject_cast<QMenu *>(widget)) {
        widget->setProperty(kMenuChromeSurfaceProperty, true);
        if (!menu->property(kMenuChromeSyncInstalledProperty).toBool()) {
            menu->installEventFilter(&MenuChromeSynchronizer::instance());
            menu->setProperty(kMenuChromeSyncInstalledProperty, true);
        }
        // `font`, not menu->font(): see syncMenuChromeSurfaces -- the menu's own
        // font is a Qt class default that its children never inherit, so taking
        // it here is what split the row's family from the entries around it.
        applyMenuFont(widget, font);
        for (QWidget *descendant : widget->findChildren<QWidget *>())
            applyMenuFont(descendant, font);
        return;
    }

    if (widget->font() != font)
        widget->setFont(font);
}

void Typography::applyChromeFontToTree(QWidget *root, const QFont &font) {
    if (!root)
        return;
    applyChromeFont(root, font);
    for (QWidget *descendant : root->findChildren<QWidget *>()) {
        if (descendant->font() != font)
            descendant->setFont(font);
    }
}

void Typography::refreshOpenMenuChrome(QMenu *menu) {
    syncMenuChromeSurfaces(menu);
}
