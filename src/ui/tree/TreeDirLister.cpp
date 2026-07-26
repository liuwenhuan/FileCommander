#include "TreeDirLister.h"

#include <QDir>
#include <QTimer>

#include "filesystem/FileProvider.h"
#include "network/NetworkSession.h"

LocalTreeLister::LocalTreeLister(QObject *parent) : TreeDirLister(parent) {}

void LocalTreeLister::requestDirs(quint64 token, const QString &path) {
    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
    if (m_showHidden)
        filters |= QDir::Hidden;
    // See the header: this scan is synchronous on the GUI thread, which is fine
    // for local disks and removable volumes but can stall on a wedged network
    // mount. The result is still delivered through the event loop below so the
    // model never re-enters itself from inside fetchMore().
    const QStringList names = QDir(path).entryList(filters, QDir::Name | QDir::LocaleAware);

    QTimer::singleShot(0, this, [this, token, path, names] {
        emit dirsListed(token, path, names);
    });
}

QString LocalTreeLister::childPath(const QString &parent, const QString &name) const {
    if (parent.endsWith(QLatin1Char('/')))
        return parent + name;
    return parent + QLatin1Char('/') + name;
}

NetworkTreeLister::NetworkTreeLister(std::weak_ptr<NetworkSession> session, QObject *parent)
    : TreeDirLister(parent), m_session(std::move(session)) {
    if (auto live = m_session.lock()) {
        connect(live.get(), &NetworkSession::listReady, this,
                &NetworkTreeLister::onSessionListReady);
        connect(live.get(), &NetworkSession::listFailed, this,
                &NetworkTreeLister::onSessionListFailed);
    }
}

void NetworkTreeLister::requestDirs(quint64 token, const QString &path) {
    auto live = m_session.lock();
    if (!live) {
        // The owning tab dropped the connection; report failure rather than
        // leaving the node spinning forever.
        QTimer::singleShot(0, this, [this, token, path] { emit listFailed(token, path); });
        return;
    }
    const quint64 reqId = m_nextReqId++;
    m_inFlight.insert(reqId, token);
    live->requestList(reqId, path, m_showHidden);
}

void NetworkTreeLister::onSessionListReady(quint64 reqId, const QString &path,
                                           const QVector<FileInfo> &entries) {
    // Whitelist, not a range test: the session emits its own post-reconnect
    // refreshes with reqId 0, and the tab's file list has ids of its own. Only
    // a request this lister issued and is still waiting for is ours.
    const auto it = m_inFlight.find(reqId);
    if (it == m_inFlight.end())
        return;
    const quint64 token = it.value();
    m_inFlight.erase(it);

    QStringList dirNames;
    for (const FileInfo &entry : entries)
        if (entry.isDir() && !entry.isParentEntry())
            dirNames.append(entry.name());
    dirNames.sort(Qt::CaseInsensitive);
    emit dirsListed(token, path, dirNames);
}

void NetworkTreeLister::onSessionListFailed(quint64 reqId, const QString &path) {
    const auto it = m_inFlight.find(reqId);
    if (it == m_inFlight.end())
        return;
    const quint64 token = it.value();
    m_inFlight.erase(it);
    emit listFailed(token, path);
}

QString NetworkTreeLister::childPath(const QString &parent, const QString &name) const {
    // Remote paths are POSIX-shaped regardless of the local platform; every
    // network provider's cleanPath() normalises to this form.
    if (parent.endsWith(QLatin1Char('/')))
        return parent + name;
    return parent + QLatin1Char('/') + name;
}
