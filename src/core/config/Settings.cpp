#include "Settings.h"

#include <QDir>
#include <QStandardPaths>

QString Settings::configDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        QStringLiteral("/totalcommander");
    QDir().mkpath(dir);
    return dir;
}

QString Settings::configFilePath() {
    return configDir() + QStringLiteral("/config.ini");
}

Settings::Settings() : m_settings(configFilePath(), QSettings::IniFormat) {
    // First run: seed the favorites with the user's home directory. Guarded by
    // a one-shot flag so clearing all favorites later doesn't re-add it.
    if (!m_settings.contains(QStringLiteral("favorites/initialized"))) {
        m_settings.setValue(QStringLiteral("favorites/directories"),
                            QStringList{QDir::homePath()});
        m_settings.setValue(QStringLiteral("favorites/initialized"), true);
    }
}

Settings::Theme Settings::theme() const {
    return static_cast<Theme>(m_settings.value("appearance/theme", 0).toInt());
}

void Settings::setTheme(Theme theme) {
    m_settings.setValue("appearance/theme", static_cast<int>(theme));
}

QString Settings::language() const {
    return m_settings.value("appearance/language", "auto").toString();
}

void Settings::setLanguage(const QString &language) {
    m_settings.setValue("appearance/language", language);
}

int Settings::listFontSize() const {
    return m_settings.value("appearance/listFontSize", 10).toInt();
}

void Settings::setListFontSize(int pt) {
    m_settings.setValue("appearance/listFontSize", qBound(7, pt, 24));
}

double Settings::videoSpeed() const {
    return m_settings.value("video/speed", 1.0).toDouble();
}

void Settings::setVideoSpeed(double speed) {
    m_settings.setValue("video/speed", speed);
}

int Settings::videoVolume() const {
    return m_settings.value("video/volume", 70).toInt();
}

void Settings::setVideoVolume(int volume) {
    m_settings.setValue("video/volume", qBound(0, volume, 100));
}

bool Settings::videoMuted() const {
    return m_settings.value("video/muted", true).toBool();
}

void Settings::setVideoMuted(bool muted) {
    m_settings.setValue("video/muted", muted);
}

bool Settings::showHiddenFiles() const {
    return m_settings.value("behavior/showHiddenFiles", false).toBool();
}

void Settings::setShowHiddenFiles(bool show) {
    m_settings.setValue("behavior/showHiddenFiles", show);
}

bool Settings::confirmDelete() const {
    return m_settings.value("behavior/confirmDelete", true).toBool();
}

void Settings::setConfirmDelete(bool confirm) {
    m_settings.setValue("behavior/confirmDelete", confirm);
}

bool Settings::confirmOverwrite() const {
    return m_settings.value("behavior/confirmOverwrite", true).toBool();
}

void Settings::setConfirmOverwrite(bool confirm) {
    m_settings.setValue("behavior/confirmOverwrite", confirm);
}

QByteArray Settings::windowGeometry() const {
    return m_settings.value("window/geometry").toByteArray();
}

void Settings::setWindowGeometry(const QByteArray &geometry) {
    m_settings.setValue("window/geometry", geometry);
}

QString Settings::functionKeyCommand(int index, const QString &defaultId) const {
    return m_settings.value(QStringLiteral("functionKeys/f%1").arg(index), defaultId).toString();
}

void Settings::setFunctionKeyCommand(int index, const QString &id) {
    m_settings.setValue(QStringLiteral("functionKeys/f%1").arg(index), id);
}

QByteArray Settings::viewHeaderState() const {
    return m_settings.value("window/viewHeaderState").toByteArray();
}

void Settings::setViewHeaderState(const QByteArray &state) {
    m_settings.setValue("window/viewHeaderState", state);
}

bool Settings::showCommandBar() const {
    return m_settings.value("view/showCommandBar", true).toBool();
}

void Settings::setShowCommandBar(bool show) {
    m_settings.setValue("view/showCommandBar", show);
}

bool Settings::showFunctionKeyBar() const {
    return m_settings.value("view/showFunctionKeyBar", true).toBool();
}

void Settings::setShowFunctionKeyBar(bool show) {
    m_settings.setValue("view/showFunctionKeyBar", show);
}

bool Settings::showFolderTree() const {
    return m_settings.value("view/showFolderTree", false).toBool();
}

void Settings::setShowFolderTree(bool show) {
    m_settings.setValue("view/showFolderTree", show);
}

QByteArray Settings::panelSplitterState() const {
    return m_settings.value("window/panelSplitterState").toByteArray();
}

void Settings::setPanelSplitterState(const QByteArray &state) {
    m_settings.setValue("window/panelSplitterState", state);
}

QByteArray Settings::outerSplitterState() const {
    return m_settings.value("window/outerSplitterState").toByteArray();
}

void Settings::setOuterSplitterState(const QByteArray &state) {
    m_settings.setValue("window/outerSplitterState", state);
}

QKeySequence Settings::shortcut(const QString &actionId, const QKeySequence &defaultSeq) const {
    const QString key = QStringLiteral("shortcuts/") + actionId;
    if (!m_settings.contains(key))
        return defaultSeq;
    return QKeySequence(m_settings.value(key).toString());
}

void Settings::setShortcut(const QString &actionId, const QKeySequence &seq) {
    m_settings.setValue(QStringLiteral("shortcuts/") + actionId, seq.toString());
}

void Settings::clearShortcutOverrides() {
    m_settings.beginGroup("shortcuts");
    m_settings.remove("");
    m_settings.endGroup();
}

QStringList Settings::favoriteDirectories() const {
    return m_settings.value("favorites/directories").toStringList();
}

void Settings::addFavoriteDirectory(const QString &path) {
    QStringList favorites = favoriteDirectories();
    if (!favorites.contains(path)) {
        favorites.append(path);
        m_settings.setValue("favorites/directories", favorites);
    }
}

void Settings::removeFavoriteDirectory(const QString &path) {
    QStringList favorites = favoriteDirectories();
    if (favorites.removeAll(path) > 0)
        m_settings.setValue("favorites/directories", favorites);
}
