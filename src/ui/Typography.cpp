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
    if (widget->font() != font)
        widget->setFont(font);
    widget->updateGeometry();
}

void syncMenuChromeSurfaces(QMenu *menu) {
    if (!menu)
        return;

    for (QWidget *child : menu->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!child->property(kMenuChromeSurfaceProperty).toBool())
            continue;
        applyMenuFont(child, menu->font());
        for (QWidget *descendant : child->findChildren<QWidget *>())
            applyMenuFont(descendant, menu->font());
        for (QAction *action : menu->actions()) {
            auto *widgetAction = qobject_cast<QWidgetAction *>(action);
            if (widgetAction && widgetAction->defaultWidget() == child &&
                widgetAction->font() != menu->font()) {
                // QAction::setFont emits changed(), which is the public signal
                // QMenu uses to invalidate its cached action geometry.
                widgetAction->setFont(menu->font());
            }
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
        if (event->type() != QEvent::FontChange &&
            event->type() != QEvent::ApplicationFontChange &&
            event->type() != QEvent::StyleChange) {
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
        applyMenuFont(widget, menu->font());
        for (QWidget *descendant : widget->findChildren<QWidget *>())
            applyMenuFont(descendant, menu->font());
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
