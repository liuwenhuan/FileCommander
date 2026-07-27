#include "Settings.h"

#include <QDir>
#include <QStandardPaths>

QString Settings::configDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        QStringLiteral("/FileCommander");
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

bool Settings::phosphorImages() const {
    return m_settings.value("appearance/phosphorImages", true).toBool();
}

void Settings::setPhosphorImages(bool on) {
    m_settings.setValue("appearance/phosphorImages", on);
}

QString Settings::language() const {
    return m_settings.value("appearance/language", "auto").toString();
}

void Settings::setLanguage(const QString &language) {
    m_settings.setValue("appearance/language", language);
}

int Settings::listFontSize() const {
    return qBound(8, m_settings.value("appearance/listFontSize", 12).toInt(), 18);
}

void Settings::setListFontSize(int pt) {
    m_settings.setValue("appearance/listFontSize", qBound(8, pt, 18));
}

QString Settings::listFontFamily() const {
    return m_settings.value("appearance/listFontFamily").toString();
}

void Settings::setListFontFamily(const QString &family) {
    m_settings.setValue("appearance/listFontFamily", family);
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

int Settings::audioVolume() const {
    return m_settings.value("audio/volume", 70).toInt();
}

void Settings::setAudioVolume(int volume) {
    m_settings.setValue("audio/volume", qBound(0, volume, 100));
}

bool Settings::audioMuted() const {
    // Default false: an audio preview is opened to be heard.
    return m_settings.value("audio/muted", false).toBool();
}

void Settings::setAudioMuted(bool muted) {
    m_settings.setValue("audio/muted", muted);
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

QString Settings::columnBaseWidths(const QString &side) const {
    return m_settings.value(QStringLiteral("view/columnWidths/%1").arg(side)).toString();
}

void Settings::setColumnBaseWidths(const QString &side, const QString &csv) {
    m_settings.setValue(QStringLiteral("view/columnWidths/%1").arg(side), csv);
}

int Settings::hiddenColumnsMask(const QString &side) const {
    return m_settings.value(QStringLiteral("view/columnHidden/%1").arg(side), -1).toInt();
}

void Settings::setHiddenColumnsMask(const QString &side, int mask) {
    m_settings.setValue(QStringLiteral("view/columnHidden/%1").arg(side), mask);
}

int Settings::sortColumn(const QString &side) const {
    return m_settings.value(QStringLiteral("view/sortColumn/%1").arg(side), -1).toInt();
}

void Settings::setSortColumn(const QString &side, int column) {
    m_settings.setValue(QStringLiteral("view/sortColumn/%1").arg(side), column);
}

int Settings::sortOrder(const QString &side) const {
    return m_settings.value(QStringLiteral("view/sortOrder/%1").arg(side), 0).toInt();
}

void Settings::setSortOrder(const QString &side, int order) {
    m_settings.setValue(QStringLiteral("view/sortOrder/%1").arg(side), order);
}

bool Settings::showCommandBar() const {
    return m_settings.value("view/showCommandBar", true).toBool();
}

void Settings::setShowCommandBar(bool show) {
    m_settings.setValue("view/showCommandBar", show);
}

bool Settings::archiveAsFolder() const {
    return m_settings.value("view/archiveAsFolder", true).toBool();
}

void Settings::setArchiveAsFolder(bool on) {
    m_settings.setValue("view/archiveAsFolder", on);
}

bool Settings::showFunctionKeyBar() const {
    return m_settings.value("view/showFunctionKeyBar", true).toBool();
}

void Settings::setShowFunctionKeyBar(bool show) {
    m_settings.setValue("view/showFunctionKeyBar", show);
}

bool Settings::showTabBar() const {
    return m_settings.value("view/showTabBar", true).toBool();
}

void Settings::setShowTabBar(bool show) {
    m_settings.setValue("view/showTabBar", show);
}

bool Settings::showShortcutLabels() const {
    return m_settings.value("view/showShortcutLabels", true).toBool();
}

void Settings::setShowShortcutLabels(bool show) {
    m_settings.setValue("view/showShortcutLabels", show);
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

int Settings::maxConcurrentTransfers() const {
    return qBound(1, m_settings.value("network/maxConcurrentTransfers", 2).toInt(), 8);
}

void Settings::setMaxConcurrentTransfers(int count) {
    m_settings.setValue("network/maxConcurrentTransfers", qBound(1, count, 8));
}

int Settings::thumbnailIconSize(const QString &side) const {
    return m_settings.value(QStringLiteral("view/thumbnailIconSize/%1").arg(side), 0).toInt();
}

void Settings::setThumbnailIconSize(const QString &side, int px) {
    m_settings.setValue(QStringLiteral("view/thumbnailIconSize/%1").arg(side), px);
}

int Settings::thumbnailCacheLimitMb() const {
    return qBound(64, m_settings.value("thumbnails/diskCacheLimitMb", 512).toInt(), 8192);
}

void Settings::setThumbnailCacheLimitMb(int mb) {
    m_settings.setValue("thumbnails/diskCacheLimitMb", qBound(64, mb, 8192));
}

int Settings::listRowHeight(const QString &side) const {
    return m_settings.value(QStringLiteral("view/listRowHeight/%1").arg(side), 0).toInt();
}

void Settings::setListRowHeight(const QString &side, int height) {
    m_settings.setValue(QStringLiteral("view/listRowHeight/%1").arg(side), height);
}

QString Settings::extraKeyCommand(const QString &slot, const QString &defaultId) const {
    return m_settings.value(QStringLiteral("functionKeys/%1").arg(slot), defaultId).toString();
}

void Settings::setExtraKeyCommand(const QString &slot, const QString &id) {
    m_settings.setValue(QStringLiteral("functionKeys/%1").arg(slot), id);
}

bool Settings::autoOpenNewDevice() const {
    return m_settings.value("behavior/autoOpenNewDevice", false).toBool();
}

void Settings::setAutoOpenNewDevice(bool on) {
    m_settings.setValue("behavior/autoOpenNewDevice", on);
}

QString Settings::updateLastCheckDate() const {
    return m_settings.value("update/lastCheckDate").toString();
}

void Settings::setUpdateLastCheckDate(const QString &date) {
    m_settings.setValue("update/lastCheckDate", date);
}

bool Settings::autoUpdateCheck() const {
    return m_settings.value("update/autoCheck", true).toBool();
}

void Settings::setAutoUpdateCheck(bool on) {
    m_settings.setValue("update/autoCheck", on);
}

bool Settings::notepadVisible() const {
    return m_settings.value("view/notepadVisible", false).toBool();
}

void Settings::setNotepadVisible(bool on) {
    m_settings.setValue("view/notepadVisible", on);
}

int Settings::notepadEditorHeight() const {
    // 0 = never set; NotepadPanel falls back to its preferred editor height.
    return m_settings.value("view/notepadEditorHeight", 0).toInt();
}

void Settings::setNotepadEditorHeight(int height) {
    m_settings.setValue("view/notepadEditorHeight", qMax(0, height));
}
