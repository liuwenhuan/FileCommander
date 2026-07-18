#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

struct SessionTabData {
    QString path;
    QStringList selectedFiles;
};

struct SessionPanelData {
    QVector<SessionTabData> tabs;
    int activeTab = 0;
};

// Persists each panel's open tabs (path + selection) across restarts,
// independent of the UI layer -- MainWindow converts to/from FilePanel's
// own tab representation.
class SessionManager {
public:
    static void save(const SessionPanelData &left, const SessionPanelData &right);

    // Returns false (leaving left/right untouched) if there's no saved
    // session yet, e.g. first launch.
    static bool load(SessionPanelData &left, SessionPanelData &right);
};
