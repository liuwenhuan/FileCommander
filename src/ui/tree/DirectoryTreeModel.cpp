#include "DirectoryTreeModel.h"

#include "TreeDirLister.h"
#include "filesystem/FileInfo.h"
#include "filesystem/IconCache.h"

namespace {

// Splits a path into its segments, independent of the local platform: remote
// paths are always POSIX-shaped and local ones are POSIX here too.
QStringList segmentsOf(const QString &path) {
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

} // namespace

DirectoryTreeModel::DirectoryTreeModel(QObject *parent) : QAbstractItemModel(parent) {}

DirectoryTreeModel::~DirectoryTreeModel() {
    for (Node *root : m_roots)
        deleteNodeTree(root);
}

void DirectoryTreeModel::deleteNodeTree(Node *node) {
    if (!node)
        return;
    for (Node *child : node->children)
        deleteNodeTree(child);
    // Any fetch still outstanding for this node must stop resolving to it.
    if (node->loading)
        m_pending.remove(node->pendingToken);
    delete node;
}

void DirectoryTreeModel::setRoots(const QVector<TreeRoot> &roots, const ListerFactory &listerFor) {
    beginResetModel();

    for (Node *root : m_roots)
        deleteNodeTree(root);
    m_roots.clear();
    qDeleteAll(m_listers);
    m_listers.clear();
    m_pending.clear();
    m_iconCache.clear();
    m_rootSpecs = roots;

    for (int i = 0; i < roots.size(); ++i) {
        const TreeRoot &spec = roots.at(i);
        auto *node = new Node;
        node->name = spec.label;
        node->path = spec.basePath;
        node->rootIndex = i;
        node->iconName = spec.iconName;
        node->activatable = spec.activatable;
        node->isRoot = true;
        node->fallbacks = spec.basePathFallbacks;
        m_roots.append(node);

        TreeDirLister *lister = listerFor ? listerFor(spec) : nullptr;
        if (lister) {
            lister->setParent(this);
            connect(lister, &TreeDirLister::dirsListed, this, &DirectoryTreeModel::onDirsListed);
            connect(lister, &TreeDirLister::listFailed, this, &DirectoryTreeModel::onListFailed);
        }
        m_listers.append(lister);
    }

    endResetModel();
}

void DirectoryTreeModel::setShowHidden(bool show) {
    m_showHidden = show;
}

void DirectoryTreeModel::refreshIcons() {
    m_iconCache.clear();
    refreshIconsBelow({});
}

void DirectoryTreeModel::refreshIconsBelow(const QModelIndex &parent) {
    const int count = rowCount(parent);
    if (count == 0)
        return;

    const QModelIndex first = index(0, 0, parent);
    const QModelIndex last = index(count - 1, 0, parent);
    emit dataChanged(first, last, {Qt::DecorationRole});
    for (int row = 0; row < count; ++row)
        refreshIconsBelow(index(row, 0, parent));
}

DirectoryTreeModel::Node *DirectoryTreeModel::nodeFor(const QModelIndex &index) const {
    if (!index.isValid())
        return nullptr;
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex DirectoryTreeModel::index(int row, int column, const QModelIndex &parent) const {
    if (column != 0 || row < 0)
        return {};
    if (!parent.isValid())
        return row < m_roots.size() ? createIndex(row, column, m_roots.at(row)) : QModelIndex();
    Node *parentNode = nodeFor(parent);
    if (!parentNode || row >= parentNode->children.size())
        return {};
    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex DirectoryTreeModel::parent(const QModelIndex &child) const {
    Node *node = nodeFor(child);
    if (!node || !node->parent)
        return {};
    Node *parentNode = node->parent;
    Node *grandParent = parentNode->parent;
    const int row = grandParent ? grandParent->children.indexOf(parentNode)
                                : m_roots.indexOf(parentNode);
    if (row < 0)
        return {};
    return createIndex(row, 0, parentNode);
}

int DirectoryTreeModel::rowCount(const QModelIndex &parent) const {
    if (!parent.isValid())
        return m_roots.size();
    Node *node = nodeFor(parent);
    return node ? node->children.size() : 0;
}

int DirectoryTreeModel::columnCount(const QModelIndex &) const {
    return 1;
}

QIcon DirectoryTreeModel::iconForNode(const Node *node) const {
    const QString key = node->isRoot && !node->iconName.isEmpty()
                            ? QStringLiteral("root:") + node->iconName
                            : QStringLiteral("folder");
    const auto cached = m_iconCache.constFind(key);
    if (cached != m_iconCache.cend())
        return cached.value();

    const QIcon themed =
        node->isRoot && !node->iconName.isEmpty()
            ? IconCache::instance().glyphIcon(QStringLiteral(":/icons/%1.svg").arg(node->iconName))
            : IconCache::instance().iconFor(FileInfo::fromFields(node->path, node->name, 0,
                                                                 QDateTime(), true,
                                                                 QFile::Permissions()));
    m_iconCache.insert(key, themed);
    return themed;
}

QVariant DirectoryTreeModel::data(const QModelIndex &index, int role) const {
    Node *node = nodeFor(index);
    if (!node)
        return {};
    switch (role) {
    case Qt::DisplayRole:
        return node->name;
    case Qt::DecorationRole:
        return iconForNode(node);
    case Qt::ToolTipRole:
        if (!node->activatable)
            return tr("This connection belongs to the other panel. Switch to it there, or "
                      "open a new connection from the Connection Manager.");
        return node->path;
    case PathRole:
        return node->path;
    case ConnectionIdRole:
        return node->rootIndex >= 0 && node->rootIndex < m_rootSpecs.size()
                   ? m_rootSpecs.at(node->rootIndex).connectionId
                   : QString();
    case ActivatableRole:
        return node->activatable;
    default:
        return {};
    }
}

Qt::ItemFlags DirectoryTreeModel::flags(const QModelIndex &index) const {
    Node *node = nodeFor(index);
    if (!node)
        return Qt::NoItemFlags;
    // A non-activatable root stays visible (the user can see what is connected
    // elsewhere) but is not selectable or enabled, so it reads as unavailable
    // and cannot be navigated to.
    if (!node->activatable)
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool DirectoryTreeModel::hasChildren(const QModelIndex &parent) const {
    if (!parent.isValid())
        return !m_roots.isEmpty();
    Node *node = nodeFor(parent);
    if (!node)
        return false;
    if (!node->activatable)
        return false; // never descend into another panel's connection
    // Before a node has been fetched, claim children so the expander arrow is
    // offered: finding out for real would mean listing the directory, which is
    // exactly the work lazy loading exists to avoid.
    return node->loaded ? !node->children.isEmpty() : true;
}

bool DirectoryTreeModel::canFetchMore(const QModelIndex &parent) const {
    if (!parent.isValid())
        return false; // roots are set explicitly, never fetched
    Node *node = nodeFor(parent);
    if (!node || node->loaded || node->loading || !node->activatable)
        return false;
    return node->rootIndex >= 0 && node->rootIndex < m_listers.size()
           && m_listers.at(node->rootIndex) != nullptr;
}

bool DirectoryTreeModel::isFetching(const QModelIndex &parent) const {
    Node *node = nodeFor(parent);
    return node && node->loading;
}

void DirectoryTreeModel::fetchMore(const QModelIndex &parent) {
    if (!canFetchMore(parent))
        return;
    Node *node = nodeFor(parent);
    TreeDirLister *lister = m_listers.at(node->rootIndex);

    // A previous fetch for this node (collapsed and re-expanded before its
    // result arrived) must not also land: retire its token first.
    if (node->pendingToken != 0)
        m_pending.remove(node->pendingToken);

    node->loading = true;
    node->pendingToken = m_nextToken++;
    m_pending.insert(node->pendingToken, node);
    lister->requestDirs(node->pendingToken, node->path);
}

DirectoryTreeModel::Node *DirectoryTreeModel::nodeAwaiting(quint64 token) const {
    return m_pending.value(token, nullptr);
}

QModelIndex DirectoryTreeModel::indexOfNode(Node *node) const {
    if (!node)
        return {};
    // Collect the chain of rows from the root down, then build the index from
    // the top: createIndex needs each level's parent to be addressable.
    QVector<int> rows;
    for (Node *cur = node; cur; cur = cur->parent) {
        const int row = cur->parent ? cur->parent->children.indexOf(cur) : m_roots.indexOf(cur);
        if (row < 0)
            return {}; // detached from the tree
        rows.prepend(row);
    }
    QModelIndex idx;
    for (int row : rows)
        idx = index(row, 0, idx);
    return idx;
}

void DirectoryTreeModel::onDirsListed(quint64 token, const QString &path,
                                      const QStringList &dirNames) {
    Q_UNUSED(path);
    Node *node = nodeAwaiting(token);
    // No node is waiting for this token: the request was superseded, or its node
    // was discarded when the roots were rebuilt. Dropping it is what stops a
    // rapid collapse/expand from inserting the children twice.
    if (!node || node->pendingToken != token)
        return;
    m_pending.remove(token);

    auto *lister = node->rootIndex >= 0 && node->rootIndex < m_listers.size()
                       ? m_listers.at(node->rootIndex)
                       : nullptr;
    if (!lister)
        return;

    const QModelIndex parentIndex = indexOfNode(node);

    // Rebuild the child list wholesale: a refetch of an already-loaded node
    // (refresh, or a re-expand after the directory changed) should reflect the
    // directory as it is now, not merge with what it used to be.
    if (!node->children.isEmpty()) {
        beginRemoveRows(parentIndex, 0, node->children.size() - 1);
        const QVector<Node *> old = node->children;
        node->children.clear();
        for (Node *child : old)
            deleteNodeTree(child);
        endRemoveRows();
    }

    if (!dirNames.isEmpty()) {
        beginInsertRows(parentIndex, 0, dirNames.size() - 1);
        for (const QString &name : dirNames) {
            auto *child = new Node;
            child->name = name;
            child->path = lister->childPath(node->path, name);
            child->rootIndex = node->rootIndex;
            child->parent = node;
            node->children.append(child);
        }
        endInsertRows();
    }

    node->loaded = true;
    node->loading = false;
    node->pendingToken = 0;
    node->fallbacks.clear(); // this level listed fine; no need to descend further
    emit childrenLoaded(parentIndex);
}

void DirectoryTreeModel::onListFailed(quint64 token, const QString &path) {
    Q_UNUSED(path);
    Node *node = nodeAwaiting(token);
    if (!node || node->pendingToken != token)
        return;
    m_pending.remove(token);
    node->loading = false;
    node->pendingToken = 0;

    // A root that could not be listed drops to its next candidate starting
    // point: the server refused this level, so the highest directory the user
    // can actually see is somewhere below it. This is what makes a network root
    // begin at the topmost VISIBLE directory rather than at a "/" nobody is
    // allowed to read.
    if (node->isRoot && !node->fallbacks.isEmpty()) {
        QString next;
        while (!node->fallbacks.isEmpty()) {
            next = node->fallbacks.takeFirst();
            if (next != node->path)
                break;
            next.clear();
        }
        if (!next.isEmpty()) {
            node->path = next;
            const QModelIndex idx = indexOfNode(node);
            emit dataChanged(idx, idx);
            fetchMore(idx);
            return;
        }
    }

    // Nothing left to try: mark it loaded so the tree stops offering to expand a
    // directory it cannot read, rather than retrying on every mouse click.
    node->loaded = true;
    emit childrenLoaded(indexOfNode(node));
}

QString DirectoryTreeModel::pathAt(const QModelIndex &index) const {
    Node *node = nodeFor(index);
    return node ? node->path : QString();
}

QString DirectoryTreeModel::connectionIdAt(const QModelIndex &index) const {
    return data(index, ConnectionIdRole).toString();
}

bool DirectoryTreeModel::isActivatable(const QModelIndex &index) const {
    Node *node = nodeFor(index);
    return node && node->activatable;
}

QModelIndex DirectoryTreeModel::indexForPath(const QString &connectionId,
                                             const QString &path) const {
    const QModelIndex deepest = deepestLoadedAncestor(connectionId, path);
    if (!deepest.isValid())
        return {};
    Node *node = nodeFor(deepest);
    return node && node->path == path ? deepest : QModelIndex();
}

QModelIndex DirectoryTreeModel::deepestLoadedAncestor(const QString &connectionId,
                                                      const QString &path) const {
    QModelIndex best;
    int bestDepth = -1;

    for (int r = 0; r < m_roots.size(); ++r) {
        if (m_rootSpecs.at(r).connectionId != connectionId)
            continue;
        Node *root = m_roots.at(r);
        // The path must live under this root for the root to be a candidate.
        if (!(path == root->path || path.startsWith(root->path.endsWith(QLatin1Char('/'))
                                                        ? root->path
                                                        : root->path + QLatin1Char('/'))))
            continue;

        QModelIndex current = index(r, 0);
        Node *node = root;
        // Walk down through children that are already materialised. Stop at the
        // first level that has not been fetched: going further would mean
        // listing a remote directory from here, which must stay the caller's
        // (asynchronous) decision.
        while (node->path != path) {
            Node *next = nullptr;
            int nextRow = -1;
            for (int i = 0; i < node->children.size(); ++i) {
                Node *child = node->children.at(i);
                if (path == child->path || path.startsWith(child->path + QLatin1Char('/'))) {
                    next = child;
                    nextRow = i;
                    break;
                }
            }
            if (!next)
                break;
            current = index(nextRow, 0, current);
            node = next;
        }
        const int depth = segmentsOf(node->path).size();
        if (depth > bestDepth) {
            best = current;
            bestDepth = depth;
        }
    }
    return best;
}
