#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include "network/ConnectionStore.h" // SavedConnection

struct SessionTabData {
    QString path;
    QStringList selectedFiles;
    // A network tab's reconnect descriptor (conn.host empty => local tab), so the
    // server and its tab label are re-established on next launch.
    SavedConnection conn;
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
