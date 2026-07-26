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

    // Removes a panel's network tabs (non-empty conn.host), keeping activeTab on
    // a survivor. Restoring one means reconnecting during startup -- a password
    // prompt, a long timeout, or a server that's simply gone -- so they are
    // dropped when the session is applied rather than when it is written:
    // save() still records them, leaving "which servers were open last time"
    // available on disk for anything else that wants it.
    //
    // Deliberately not folded into load(), which stays a faithful reader of the
    // file; this is a policy the caller opts into. A panel can come back empty
    // (every tab was a network tab) -- falling back to a local path is the
    // caller's call, since only it knows what a sensible default is.
    static void dropNetworkTabs(SessionPanelData &panel);
};
