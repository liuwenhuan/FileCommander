#pragma once

#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

#include "network/ConnectionStore.h" // SavedConnection (per-tab reconnect descriptor)

class FileProvider;
class NetworkSession;

// Per-tab state. Deliberately stored via QSharedPointer<TabState> in a
// QVector (never by value in a QList) -- the old project's TabManager
// crashed on destruction from exactly that pattern (double-free of the
// QStringList members when a QList<TabState> tore itself down).
struct TabState {
    QString path;
    QStringList selectedFiles;
    int sortColumn = 0;
    int sortOrder = 0; // Qt::AscendingOrder / Qt::DescendingOrder
    // A non-empty flatPaths marks this tab as a flat search-results listing (a
    // temporary virtual directory) rather than a real directory; title is the
    // custom tab label (the search keyword) shown instead of a path-derived one.
    QStringList flatPaths;
    QString title;

    // This tab was showing the computer view when it went to the background.
    // Unlike an archive browse -- which holds a downloaded file and a parked
    // connection, and so cannot travel with a backgrounded tab -- the computer
    // view is only a snapshot of places, so it is cheap to rebuild and the tab
    // is expected to come back to it. `path` still records the directory the
    // view was opened from, so anything that can only deal with a real path
    // (the shutdown snapshot, a restart) has one.
    bool computerView = false;

    // Per-tab remote connection. Each tab owns its OWN connection so switching
    // tabs (or browsing a local folder in a sibling tab) never drops another
    // tab's server. All empty/null for a plain local tab. `provider`/`session`
    // hold the live backend while the tab is inactive (the model borrows them
    // for the active tab); `connScheme` drives the tab's protocol icon and
    // `connLabel` ("user@host") its label prefix, both persisting independently
    // of whatever backend is momentarily active. `authLabel`/`authFactory` carry
    // the credential-retry context so a parked connection can still re-auth.
    std::shared_ptr<FileProvider> provider;
    std::shared_ptr<NetworkSession> session;
    QString connScheme;
    QString connLabel;
    QString authLabel;
    std::function<std::function<bool(QString *)>(const QString &, const QString &)> authFactory;
    // Enough to re-establish this connection after a restart (protocol/host/port/
    // user/remotePath, and the bookmark id for its keyring password). connInfo.host
    // empty => a local tab. Persisted in the session so the server -- and its tab
    // label -- come back on next launch instead of silently falling back to a
    // local directory. [[ttc-per-tab-connections]]
    SavedConnection connInfo;
};

// Owns the tab state for one FilePanel. Purely data/state -- the visual
// QTabBar (see TabBar.h) is a separate widget that FilePanel keeps in
// sync with this.
class TabManager : public QObject {
    Q_OBJECT

public:
    explicit TabManager(QObject *parent = nullptr);

    int addTab(const QString &path);
    void closeTab(int index);
    void closeOthers(int index);
    void closeToRight(int index);

    void setActiveIndex(int index);
    int activeIndex() const { return m_activeIndex; }
    int count() const { return m_tabs.size(); }

    QSharedPointer<TabState> tabAt(int index) const;
    QSharedPointer<TabState> activeTab() const;

signals:
    void tabAdded(int index);
    void tabClosed(int index);
    void activeChanged(int index);

private:
    QVector<QSharedPointer<TabState>> m_tabs;
    int m_activeIndex = -1;
};
