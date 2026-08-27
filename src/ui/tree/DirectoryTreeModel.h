#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "TreeRootBuilder.h"

class TreeDirLister;

// The folder tree's model: a lazily-populated directory tree whose top level is
// a set of TreeRoots (local disks, removable volumes, live server connections)
// rather than a single filesystem root.
//
// QFileSystemModel cannot back this -- it only knows the local filesystem and
// has no way to graft a remote subtree in. Instead each root carries its own
// TreeDirLister, and a node's children are fetched the first time it is
// expanded. Nothing is ever walked recursively: a collapsed subtree costs
// nothing, which is what keeps a remote connection usable.
class DirectoryTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role {
        PathRole = Qt::UserRole + 1, // absolute (or provider-relative) path
        ConnectionIdRole,            // network nodes: owning connection, else empty
        ActivatableRole,             // false => shown but not navigable
    };

    explicit DirectoryTreeModel(QObject *parent = nullptr);
    ~DirectoryTreeModel() override;

    // Replaces the top level. Listers are supplied per root: `listerFor` is
    // called once per root and the model takes ownership of what it returns
    // (returning null makes that root a leaf). Expansion state of surviving
    // roots is preserved by path, so a hot-plug elsewhere does not collapse the
    // tree the user is working in.
    using ListerFactory = std::function<TreeDirLister *(const TreeRoot &)>;
    void setRoots(const QVector<TreeRoot> &roots, const ListerFactory &listerFor);

    // Whether hidden directories are listed. Applies to subsequent fetches.
    void setShowHidden(bool show);

    // Re-emits existing decorations after an icon-theme treatment changes without
    // resetting nodes, so expanded branches remain expanded.
    void refreshIcons();

    // The path behind an index, or an empty string for an invalid one.
    QString pathAt(const QModelIndex &index) const;
    // The connection an index belongs to ("" for local nodes).
    QString connectionIdAt(const QModelIndex &index) const;
    bool isActivatable(const QModelIndex &index) const;

    // Finds the index for `path` within the root owning `connectionId`, without
    // fetching anything: only nodes already materialised are considered. Returns
    // an invalid index when the path is not currently in the tree.
    QModelIndex indexForPath(const QString &connectionId, const QString &path) const;

    // The deepest already-materialised ancestor of `path`, plus how much of the
    // path remains below it. Used to walk the tree towards a path one asynchronous
    // level at a time instead of blocking on a recursive descent.
    QModelIndex deepestLoadedAncestor(const QString &connectionId, const QString &path) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    bool isFetching(const QModelIndex &parent) const;
    void fetchMore(const QModelIndex &parent) override;

signals:
    // A node finished loading its children. Drives the "expand towards a path"
    // walk in FilePanel, which cannot know when a remote level has landed.
    void childrenLoaded(const QModelIndex &parent);

private:
    struct Node {
        QString name;
        QString path;
        int rootIndex = -1; // which root (and therefore which lister) this is under
        Node *parent = nullptr;
        QVector<Node *> children;
        bool loaded = false;  // children have been fetched at least once
        bool loading = false; // a fetch is in flight
        // The token of the in-flight fetch. A later fetch bumps it, so a result
        // from a superseded request (collapse, re-expand, refresh) is recognised
        // and dropped instead of appending a second copy of the children.
        quint64 pendingToken = 0;
        // Roots render a device/connection icon; everything below is a folder.
        QString iconName;
        bool activatable = true;
        bool isRoot = false;
        // Roots only: successively deeper starting points to try when `path`
        // turns out not to be listable (see TreeRoot::basePathFallbacks). Each
        // failure consumes the head of this list and retries one level down.
        QStringList fallbacks;
    };

    void onDirsListed(quint64 token, const QString &path, const QStringList &dirNames);
    void onListFailed(quint64 token, const QString &path);
    // Locates the node awaiting `token`, or null if none is (a stale result).
    Node *nodeAwaiting(quint64 token) const;
    // The index addressing `node`, walking up to its root. Invalid if the node
    // is no longer attached to the tree.
    QModelIndex indexOfNode(Node *node) const;
    void deleteNodeTree(Node *node);
    void refreshIconsBelow(const QModelIndex &parent);
    QIcon iconForNode(const Node *node) const;
    Node *nodeFor(const QModelIndex &index) const;

    QVector<Node *> m_roots;
    QVector<TreeRoot> m_rootSpecs;
    QVector<TreeDirLister *> m_listers; // parallel to m_roots; owned by this
    mutable QHash<QString, QIcon> m_iconCache;
    quint64 m_nextToken = 1;
    bool m_showHidden = false;
    // Nodes with an outstanding fetch, by token, so a result finds its node in
    // constant time and a result for a node that has since been discarded (its
    // root was removed) simply finds nothing.
    QHash<quint64, Node *> m_pending;
};
