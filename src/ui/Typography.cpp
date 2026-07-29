#include "Typography.h"

#include <QApplication>
#include <QFontDatabase>
#include <QWidget>

#include "Settings.h"

namespace {

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
    QApplication::setFont(chromeFont(settings));
}

void Typography::applyChromeFont(QWidget *widget, const Settings &settings) {
    if (widget)
        widget->setFont(chromeFont(settings));
}
