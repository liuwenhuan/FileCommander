#include "SessionManager.h"

#include "Settings.h"

namespace {

// Session state lives in its own file (it changes on every tab move, unlike the
// stable config.ini) but shares the config directory — one source of truth.
QString sessionFilePath() {
    return Settings::configDir() + QStringLiteral("/session.ini");
}

void savePanel(QSettings &settings, const QString &group, const SessionPanelData &panel) {
    settings.beginGroup(group);
    settings.setValue("activeTab", panel.activeTab);
    settings.beginWriteArray("tabs");
    for (int i = 0; i < panel.tabs.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("path", panel.tabs.at(i).path);
        settings.setValue("selectedFiles", panel.tabs.at(i).selectedFiles);
    }
    settings.endArray();
    settings.endGroup();
}

bool loadPanel(QSettings &settings, const QString &group, SessionPanelData &panel) {
    settings.beginGroup(group);
    const int count = settings.beginReadArray("tabs");
    if (count == 0) {
        settings.endArray();
        settings.endGroup();
        return false;
    }
    panel.tabs.clear();
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        SessionTabData tab;
        tab.path = settings.value("path").toString();
        tab.selectedFiles = settings.value("selectedFiles").toStringList();
        panel.tabs.append(tab);
    }
    settings.endArray();
    panel.activeTab = settings.value("activeTab", 0).toInt();
    settings.endGroup();
    return true;
}

} // namespace

void SessionManager::save(const SessionPanelData &left, const SessionPanelData &right) {
    QSettings settings(sessionFilePath(), QSettings::IniFormat);
    savePanel(settings, "leftPanel", left);
    savePanel(settings, "rightPanel", right);
}

bool SessionManager::load(SessionPanelData &left, SessionPanelData &right) {
    QSettings settings(sessionFilePath(), QSettings::IniFormat);
    const bool leftOk = loadPanel(settings, "leftPanel", left);
    const bool rightOk = loadPanel(settings, "rightPanel", right);
    return leftOk && rightOk;
}
