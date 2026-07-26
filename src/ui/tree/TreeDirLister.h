#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

// FileInfo arrives by value in the NetworkSession::listReady signature this
// lister connects to, so moc needs the full type.
#include "filesystem/FileInfo.h"

class NetworkSession;

// Lists the subdirectories of one directory for DirectoryTreeModel. One lister
// backs one data source: the local filesystem, or a single server connection.
//
// Listing is modelled as asynchronous throughout, even where a backend answers
// immediately, so the model has exactly one code path. `token` is echoed back
// with the result and lets the model discard a superseded request.
class TreeDirLister : public QObject {
    Q_OBJECT

public:
    explicit TreeDirLister(QObject *parent = nullptr) : QObject(parent) {}

    // Requests the subdirectory names of `path`. Exactly one of dirsListed /
    // listFailed must eventually follow, carrying the same token.
    virtual void requestDirs(quint64 token, const QString &path) = 0;

    // Joins a parent directory and a child name the way this backend does.
    virtual QString childPath(const QString &parent, const QString &name) const = 0;

signals:
    void dirsListed(quint64 token, const QString &path, const QStringList &dirNames);
    void listFailed(quint64 token, const QString &path);
};

// Local filesystem source. QDir answers immediately, so the result is delivered
// through a queued self-invocation to keep the model's contract asynchronous
// (never re-entering the model from inside its own fetchMore).
//
// Known limitation, accepted for now: the QDir scan runs on the GUI thread. On
// a wedged autofs/NFS mount, expanding that directory can briefly block the UI
// -- QFileSystemModel avoided this with its own worker thread. Local disks and
// USB volumes, which is what this tree is for, answer in microseconds, and
// putting the local path behind a thread pool as well would add a whole
// asynchronous lifetime to a change that already spans several files. Revisit
// if network-mounted local paths turn out to matter in practice.
class LocalTreeLister : public TreeDirLister {
    Q_OBJECT

public:
    explicit LocalTreeLister(QObject *parent = nullptr);

    void setShowHidden(bool show) { m_showHidden = show; }

    void requestDirs(quint64 token, const QString &path) override;
    QString childPath(const QString &parent, const QString &name) const override;

private:
    bool m_showHidden = false;
};

// Server source, backed by the SAME NetworkSession the tab's file list uses.
// That is deliberate: the session is a single-threaded actor, so the tree's
// listings queue behind the file list's instead of racing them -- which is what
// makes this safe for libsmbclient, whose global state cannot survive
// concurrent use, without any extra locking.
//
// Request ids are namespaced into the high half of the session's quint64 id
// space so the tab's FileSystemModel discards them (its own ids start at 1 and
// count up), and this lister in turn ignores anything that isn't one of its own.
class NetworkTreeLister : public TreeDirLister {
    Q_OBJECT

public:
    // The session is held weakly: the owning tab, not the tree, decides how long
    // the connection lives.
    NetworkTreeLister(std::weak_ptr<NetworkSession> session, QObject *parent = nullptr);

    void setShowHidden(bool show) { m_showHidden = show; }

    void requestDirs(quint64 token, const QString &path) override;
    QString childPath(const QString &parent, const QString &name) const override;

    // Marks the high half of the id space, which is where every tree request
    // lives. Public so the namespacing can be asserted in tests.
    static constexpr quint64 kTreeReqIdBase = quint64(1) << 63;

private slots:
    void onSessionListReady(quint64 reqId, const QString &path, const QVector<FileInfo> &entries);
    void onSessionListFailed(quint64 reqId, const QString &path);

private:
    std::weak_ptr<NetworkSession> m_session;
    // Tokens this lister has issued and is still waiting for, mapped from the
    // namespaced session request id. A whitelist rather than a range test: the
    // session also emits self-initiated refreshes (reqId 0) after a reconnect,
    // which belong to neither the tree nor the file list.
    QHash<quint64, quint64> m_inFlight; // session reqId -> model token
    quint64 m_nextReqId = kTreeReqIdBase + 1;
    bool m_showHidden = false;
};
