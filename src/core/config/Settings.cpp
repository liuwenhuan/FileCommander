#include "Settings.h"

#include "text/TextEncodingDetector.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "PrivatePath.h"

namespace {

QString prepareConfigDirectory(const QString &basePath) {
    if (basePath.isEmpty())
        return QString();

    const QString base = QDir::cleanPath(QFileInfo(basePath).absoluteFilePath());
    if (base == QStringLiteral("/"))
        return QString();

    const QString directory = QDir(base).filePath(QStringLiteral("FileCommander"));
    if (!QDir().mkpath(directory))
        return QString();

    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir() || directoryInfo.isSymbolicLink())
        return QString();

    const QString canonicalDirectory = directoryInfo.canonicalFilePath();
    const QString canonicalBase = QFileInfo(base).canonicalFilePath();
    if (canonicalDirectory.isEmpty() || canonicalDirectory == QStringLiteral("/") ||
        canonicalBase.isEmpty() || canonicalBase == QStringLiteral("/") ||
        !canonicalDirectory.startsWith(canonicalBase + QLatin1Char('/'))) {
        return QString();
    }
    if (!PrivatePath::restrictDirectory(canonicalDirectory)) {
        return QString();
    }
    return canonicalDirectory;
}

} // namespace

QString Settings::configDir() {
    const QString override = qEnvironmentVariable("FILECOMMANDER_CONFIG_HOME");
    if (!override.isEmpty())
        return prepareConfigDirectory(override);

    const QString standardBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString standardDirectory = prepareConfigDirectory(standardBase);
    if (!standardDirectory.isEmpty())
        return standardDirectory;

    const QString home = QDir::homePath();
    if (home.isEmpty() || QDir::cleanPath(QFileInfo(home).absoluteFilePath()) == QStringLiteral("/"))
        return QString();
    return prepareConfigDirectory(QDir(home).filePath(QStringLiteral(".config")));
}

QString Settings::configFilePath() {
    const QString dir = configDir();
    return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("config.ini"));
}

namespace {
constexpr int kMaximumRememberedTextEncodings = 256;
const QString kTextEncodingOverrides = QStringLiteral("textEncoding/overrides/");
const QString kTextEncodingOrder = QStringLiteral("textEncoding/overrideOrder");

QString textEncodingIdentityHash(const QString &identity) {
    if (identity.isEmpty())
        return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool isManualTextEncodingIndex(int index) {
    return index > TextEncodingDetector::autoEncodingIndex &&
           index < TextEncodingDetector::selectableEncodingCount;
}

QString normalizedAccountServerUrl(QString url) {
    url = url.trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return url;
}
} // namespace

Settings::Settings() : Settings(configFilePath()) {}

Settings::Settings(const QString &iniFilePath)
    : m_settings(iniFilePath.isEmpty() ? configFilePath() : iniFilePath, QSettings::IniFormat) {
    // Motion is no longer user-configurable. Remove the retired preference so
    // an older `true` value cannot silently disable animation after upgrade.
    m_settings.remove(QStringLiteral("appearance/reduceMotion"));

    // The old free-form field used an empty value for the compiled server and
    // persisted the retired official host as if it were custom. Make the choice
    // explicit once, preserving genuinely self-hosted endpoints.
    if (!m_settings.contains(QStringLiteral("account/serverMode"))) {
        const QString legacy =
            normalizedAccountServerUrl(m_settings.value(QStringLiteral("account/serverUrl")).toString());
        const bool official =
            legacy.isEmpty() ||
            legacy.compare(QStringLiteral("https://sgvps.aigutta.com"), Qt::CaseInsensitive) == 0;
        m_settings.setValue(QStringLiteral("account/serverMode"),
                            official ? QStringLiteral("official") : QStringLiteral("custom"));
        if (!official)
            m_settings.setValue(QStringLiteral("account/customServerUrl"), legacy);
    }
    m_settings.remove(QStringLiteral("account/serverUrl"));
    m_settings.remove(QStringLiteral("account/rememberAutoLogin"));
    // The redesigned clipboard only keeps local history and explicit deliveries.
    // Remove automatic-mirroring preferences rather than preserving dormant data
    // an older UI could accidentally reactivate after an upgrade.
    m_settings.remove(QStringLiteral("account/cloudClipboardAutoUpload"));
    m_settings.remove(QStringLiteral("account/cloudClipboardAutoReceive"));
    m_settings.remove(QStringLiteral("account/cloudClipboardPrivacyAcknowledged"));
    m_settings.remove(QStringLiteral("view/cloudClipboardEditorHeight"));
    // Update discovery is always scheduled; prior opt-out and shortcut values
    // are retired so no invisible setting can suppress future checks.
    m_settings.remove(QStringLiteral("update/autoCheck"));
    m_settings.remove(QStringLiteral("shortcuts/toggleAutoUpdate"));

    // First run: seed the favorites with the user's home directory. Guarded by
    // a one-shot flag so clearing all favorites later doesn't re-add it.
    if (!m_settings.contains(QStringLiteral("favorites/initialized"))) {
        m_settings.setValue(QStringLiteral("favorites/directories"),
                            QStringList{QDir::homePath()});
        m_settings.setValue(QStringLiteral("favorites/initialized"), true);
    }
    m_settings.sync();
    const QString path = m_settings.fileName();
    if (!path.isEmpty() && QFileInfo(path).isFile() && !QFileInfo(path).isSymbolicLink())
        PrivatePath::restrictFile(path);
}

Settings::~Settings() {
    m_settings.sync();
    const QString path = m_settings.fileName();
    if (!path.isEmpty() && QFileInfo(path).isFile() && !QFileInfo(path).isSymbolicLink())
        PrivatePath::restrictFile(path);
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

bool Settings::phosphorPreview() const {
    return m_settings.value("appearance/phosphorPreview", true).toBool();
}

void Settings::setPhosphorPreview(bool on) {
    m_settings.setValue("appearance/phosphorPreview", on);
}

QString Settings::language() const {
    return m_settings.value("appearance/language", "auto").toString();
}

void Settings::setLanguage(const QString &language) {
    m_settings.setValue("appearance/language", language);
}

int Settings::rememberedTextEncodingIndex(const QString &stableIdentity) const {
    const QString hash = textEncodingIdentityHash(stableIdentity);
    if (hash.isEmpty())
        return TextEncodingDetector::autoEncodingIndex;
    bool ok = false;
    const int index = m_settings.value(kTextEncodingOverrides + hash).toInt(&ok);
    return ok && isManualTextEncodingIndex(index)
               ? index
               : TextEncodingDetector::autoEncodingIndex;
}

void Settings::setRememberedTextEncodingIndex(const QString &stableIdentity, int index) {
    const QString hash = textEncodingIdentityHash(stableIdentity);
    if (hash.isEmpty())
        return;

    const QString key = kTextEncodingOverrides + hash;
    QStringList order = m_settings.value(kTextEncodingOrder).toStringList();
    order.removeAll(hash);

    if (!isManualTextEncodingIndex(index)) {
        if (!m_settings.contains(key))
            return;
        m_settings.remove(key);
        m_settings.setValue(kTextEncodingOrder, order);
        return;
    }

    bool ok = false;
    const int existing = m_settings.value(key).toInt(&ok);
    if (ok && existing == index)
        return;

    m_settings.setValue(key, index);
    order.append(hash);
    while (order.size() > kMaximumRememberedTextEncodings) {
        const QString evicted = order.takeFirst();
        m_settings.remove(kTextEncodingOverrides + evicted);
    }
    m_settings.setValue(kTextEncodingOrder, order);
}

QString Settings::globalFontFamily() const {
    const QString selected = m_settings.value("appearance/fontFamily").toString();
    if (!selected.isEmpty())
        return selected;

    // Preserve the choice made by versions that exposed separate font selectors.
    const QString legacyList = m_settings.value("appearance/listFontFamily").toString();
    if (!legacyList.isEmpty())
        return legacyList;
    return m_settings.value("appearance/interfaceFontFamily").toString();
}

void Settings::setGlobalFontFamily(const QString &family) {
    const QString trimmed = family.trimmed();
    if (trimmed.isEmpty())
        m_settings.remove("appearance/fontFamily");
    else
        m_settings.setValue("appearance/fontFamily", trimmed);
    m_settings.remove("appearance/listFontFamily");
    m_settings.remove("appearance/interfaceFontFamily");
}

int Settings::listFontSize() const {
    return qBound(8, m_settings.value("appearance/listFontSize", 12).toInt(), 16);
}

void Settings::setListFontSize(int pt) {
    m_settings.setValue("appearance/listFontSize", qBound(8, pt, 16));
}

int Settings::menuFontSize() const {
    const int legacySize = m_settings.value("appearance/interfaceFontSize", 12).toInt();
    return qBound(8, m_settings.value("appearance/menuFontSize", legacySize).toInt(), 16);
}

void Settings::setMenuFontSize(int pt) {
    m_settings.setValue("appearance/menuFontSize", qBound(8, pt, 16));
    m_settings.remove("appearance/interfaceFontSize");
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
    return m_settings.value("video/muted", false).toBool();
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

int Settings::transferRateLimitKib() const {
    return qMax(0, m_settings.value("network/transferRateLimitKib", 0).toInt());
}

void Settings::setTransferRateLimitKib(int kib) {
    m_settings.setValue("network/transferRateLimitKib", qMax(0, kib));
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

QString Settings::updatePendingVersion() const {
    return m_settings.value("update/pendingVersion").toString();
}

QString Settings::updatePendingDate() const {
    return m_settings.value("update/pendingDate").toString();
}

QString Settings::updatePendingNotes() const {
    return m_settings.value("update/pendingNotes").toString();
}

void Settings::setPendingUpdate(const QString &version, const QString &date, const QString &notes) {
    m_settings.setValue("update/pendingVersion", version);
    m_settings.setValue("update/pendingDate", date);
    m_settings.setValue("update/pendingNotes", notes);
}

void Settings::clearPendingUpdate() {
    m_settings.remove("update/pendingVersion");
    m_settings.remove("update/pendingDate");
    m_settings.remove("update/pendingNotes");
}

QString Settings::accountEmail() const {
    return m_settings.value("account/email").toString();
}

void Settings::setAccountEmail(const QString &email) {
    m_settings.setValue("account/email", email);
}

QString Settings::accountDeviceId() const {
    return m_settings.value("account/deviceId").toString();
}

void Settings::setAccountDeviceId(const QString &id) {
    m_settings.setValue("account/deviceId", id);
}

QString Settings::accountDeviceName() const {
    return m_settings.value("account/deviceName").toString();
}

void Settings::setAccountDeviceName(const QString &name) {
    m_settings.setValue("account/deviceName", name);
}

bool Settings::accountUsesOfficialServer() const {
    return m_settings.value("account/serverMode", QStringLiteral("official")).toString() !=
           QStringLiteral("custom");
}

void Settings::setAccountUsesOfficialServer(bool official) {
    m_settings.setValue("account/serverMode",
                        official ? QStringLiteral("official") : QStringLiteral("custom"));
}

QString Settings::accountCustomServerUrl() const {
    return normalizedAccountServerUrl(m_settings.value("account/customServerUrl").toString());
}

void Settings::setAccountCustomServerUrl(const QString &url) {
    m_settings.setValue("account/customServerUrl", normalizedAccountServerUrl(url));
}

bool Settings::cloudClipboardAutoSend() const {
    return m_settings.value("account/cloudClipboardAutoSend", false).toBool();
}

void Settings::setCloudClipboardAutoSend(bool enabled) {
    m_settings.setValue("account/cloudClipboardAutoSend", enabled);
}

QString Settings::cloudClipboardTargetDeviceId() const {
    return m_settings.value("account/cloudClipboardTargetDeviceId").toString();
}

void Settings::setCloudClipboardTargetDeviceId(const QString &id) {
    m_settings.setValue("account/cloudClipboardTargetDeviceId", id.trimmed());
}

bool Settings::deviceSharingEnabled() const {
    return m_settings.value("account/sharingEnabled", false).toBool();
}

void Settings::setDeviceSharingEnabled(bool on) {
    m_settings.setValue("account/sharingEnabled", on);
}

bool Settings::notifyOnReceived() const {
    return m_settings.value("account/notifyOnReceived", true).toBool();
}

void Settings::setNotifyOnReceived(bool on) {
    m_settings.setValue("account/notifyOnReceived", on);
}

QStringList Settings::sharedFolders() const {
    return m_settings.value("account/sharedFolders").toStringList();
}

void Settings::setSharedFolders(const QStringList &folders) {
    m_settings.setValue("account/sharedFolders", folders);
}

bool Settings::showSystemVolumes() const {
    return m_settings.value("behavior/showSystemVolumes", false).toBool();
}

void Settings::setShowSystemVolumes(bool show) {
    m_settings.setValue("behavior/showSystemVolumes", show);
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
