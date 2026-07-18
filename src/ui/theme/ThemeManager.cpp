#include "ThemeManager.h"

#include <QApplication>
#include <QFile>

ThemeManager::ThemeManager(QObject *parent) : QObject(parent) {
    // Must capture this before any stylesheet is ever applied, since a
    // stylesheet can itself alter qApp->palette().
    m_originalPalette = qApp->palette();
}

bool ThemeManager::systemPrefersDark() const {
    return m_originalPalette.color(QPalette::Window).lightness() < 128;
}

void ThemeManager::apply(Settings::Theme theme) {
    m_requestedTheme = theme;

    Settings::Theme effective = theme;
    if (effective == Settings::Theme::Auto)
        effective = systemPrefersDark() ? Settings::Theme::Dark : Settings::Theme::Light;

    const QString path =
        effective == Settings::Theme::Dark ? ":/themes/dark.qss" : ":/themes/light.qss";
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
}
