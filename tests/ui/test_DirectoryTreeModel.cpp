#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVector>

#include "tree/DirectoryTreeModel.h"
#include "tree/TreeDirLister.h"
#include "filesystem/IconCache.h"

namespace {

// A lister whose results are delivered only when the test says so, which is
// what lets these tests reproduce the ordering hazards a real network
// connection creates (a result arriving after the user already collapsed and
// re-expanded the node) deterministically.
class FakeLister : public TreeDirLister {
public:
    struct Pending {
        quint64 token;
        QString path;
    };

    void requestDirs(quint64 token, const QString &path) override {
        requests.append({token, path});
    }

    QString childPath(const QString &parent, const QString &name) const override {
        return parent.endsWith('/') ? parent + name : parent + '/' + name;
    }

    // Answers the Nth outstanding request (default: the most recent).
    void deliver(const QStringList &names, int requestIndex = -1) {
        const Pending &req = requests.at(requestIndex < 0 ? requests.size() - 1 : requestIndex);
        emit dirsListed(req.token, req.path, names);
    }

    void fail(int requestIndex = -1) {
        const Pending &req = requests.at(requestIndex < 0 ? requests.size() - 1 : requestIndex);
        emit listFailed(req.token, req.path);
    }

    QVector<Pending> requests;
};

TreeRoot localRoot(const QString &path = QStringLiteral("/")) {
    TreeRoot root;
    root.kind = TreeRoot::LocalFilesystem;
    root.label = QStringLiteral("System");
    root.basePath = path;
    return root;
}

TreeRoot networkRoot(const QString &connId, const QString &base,
                     const QStringList &fallbacks = {}) {
    TreeRoot root;
    root.kind = TreeRoot::Network;
    root.label = QStringLiteral("deepin@192.168.1.2");
    root.iconName = QStringLiteral("dev-smb");
    root.basePath = base;
    root.basePathFallbacks = fallbacks;
    root.connectionId = connId;
    return root;
}

} // namespace

TEST(DirectoryTreeModelTest, RootsAppearAsTopLevelRows) {
    DirectoryTreeModel model;
    model.setRoots({localRoot(), networkRoot("smb://host", "/")},
                   [](const TreeRoot &) -> TreeDirLister * { return new FakeLister; });

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.index(0, 0).data(Qt::DisplayRole).toString(), QString("System"));
    EXPECT_EQ(model.index(1, 0).data(DirectoryTreeModel::ConnectionIdRole).toString(),
              QString("smb://host"));
}

TEST(DirectoryTreeModelTest, ThemeIconsAreCachedUntilRefresh) {
    struct TintReset {
        ~TintReset() { IconCache::instance().setTint(QColor()); }
    } tintReset;

    IconCache &icons = IconCache::instance();
    icons.setTint(Qt::red);

    DirectoryTreeModel model;
    model.setRoots({localRoot()},
                   [](const TreeRoot &) -> TreeDirLister * { return new FakeLister; });
    const QModelIndex root = model.index(0, 0);

    const QIcon first = root.data(Qt::DecorationRole).value<QIcon>();
    const QIcon repeated = root.data(Qt::DecorationRole).value<QIcon>();
    const QImage redImage = first.pixmap(32, 32).toImage();
    ASSERT_FALSE(redImage.isNull());
    EXPECT_EQ(repeated.cacheKey(), first.cacheKey());

    icons.setTint(Qt::green);
    model.refreshIcons();
    const QImage greenImage = root.data(Qt::DecorationRole)
                                  .value<QIcon>()
                                  .pixmap(32, 32)
                                  .toImage();
    ASSERT_FALSE(greenImage.isNull());
    EXPECT_NE(greenImage, redImage);

    icons.setTint(Qt::blue);
    model.setRoots({localRoot()},
                   [](const TreeRoot &) -> TreeDirLister * { return new FakeLister; });
    const QImage blueImage = model.index(0, 0)
                                 .data(Qt::DecorationRole)
                                 .value<QIcon>()
                                 .pixmap(32, 32)
                                 .toImage();
    ASSERT_FALSE(blueImage.isNull());
    EXPECT_NE(blueImage, greenImage);
}

// Nothing may be listed until a node is actually expanded: a tree that walked
// the directories up front would stall for the whole of a remote connection.
TEST(DirectoryTreeModelTest, RefreshIconsKeepsLoadedNodesAndOnlySignalsDecorationChanges) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->deliver({"home"});
    const QModelIndex child = model.index(0, 0, root);
    ASSERT_TRUE(child.isValid());

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.refreshIcons();

    ASSERT_EQ(changed.count(), 2);
    for (const QList<QVariant> &emission : changed) {
        const QVector<int> roles = emission.at(2).value<QVector<int>>();
        EXPECT_EQ(roles, QVector<int>({Qt::DecorationRole}));
    }
    EXPECT_TRUE(root.isValid());
    EXPECT_TRUE(child.isValid());
    EXPECT_EQ(model.rowCount(root), 1);
}

TEST(DirectoryTreeModelTest, NothingIsListedUntilANodeIsExpanded) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    ASSERT_NE(lister, nullptr);
    EXPECT_TRUE(lister->requests.isEmpty());
    EXPECT_EQ(model.rowCount(model.index(0, 0)), 0);

    model.fetchMore(model.index(0, 0));
    ASSERT_EQ(lister->requests.size(), 1);
    EXPECT_EQ(lister->requests.first().path, QString("/"));
}

TEST(DirectoryTreeModelTest, DeliveredChildrenBecomeRows) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->deliver({"bin", "home", "usr"});

    ASSERT_EQ(model.rowCount(root), 3);
    EXPECT_EQ(model.index(1, 0, root).data(Qt::DisplayRole).toString(), QString("home"));
    EXPECT_EQ(model.pathAt(model.index(1, 0, root)), QString("/home"));
    EXPECT_FALSE(model.canFetchMore(root)); // loaded; no repeat fetch
}

// A node with two requests in flight must accept only the current one. This is
// the duplicate-children hazard: both results name the same directory, so
// without per-node request tracking the children are inserted twice and every
// entry appears two times. The fallback path is where a node genuinely gets a
// second in-flight request (the failure of "/" issues a fresh one for "/share"),
// so a late duplicate answer to the FIRST request must be dropped.
TEST(DirectoryTreeModelTest, SupersededRequestResultIsDroppedNotAppended) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({networkRoot("smb://host", "/", {"/share"})},
                   [&](const TreeRoot &) -> TreeDirLister * {
                       lister = new FakeLister;
                       return lister;
                   });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    ASSERT_EQ(lister->requests.size(), 1);

    lister->fail(0); // request 0 fails -> request 1 goes out for "/share"
    ASSERT_EQ(lister->requests.size(), 2);

    // A duplicate answer to the superseded request 0 arrives late.
    lister->deliver({"stale-a", "stale-b"}, 0);
    EXPECT_EQ(model.rowCount(root), 0); // ignored: not the node's pending token

    // The current request answers; its children land exactly once.
    lister->deliver({"docs"}, 1);
    ASSERT_EQ(model.rowCount(root), 1);
    EXPECT_EQ(model.pathAt(model.index(0, 0, root)), QString("/share/docs"));
}

// A result carrying a token the model never issued (the session's own
// post-reconnect refresh uses reqId 0, and the tab's file list has ids of its
// own) must be ignored rather than matched by shape.
TEST(DirectoryTreeModelTest, ForeignTokenIsIgnored) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);

    emit lister->dirsListed(999999, "/", {"bin", "home"}); // never issued here
    EXPECT_EQ(model.rowCount(root), 0);

    lister->deliver({"bin", "home"}); // the model's own request still works
    EXPECT_EQ(model.rowCount(root), 2);
}

// Once a node is loaded, further fetchMore calls (which the view makes freely)
// must not re-list it, and must not stack a second copy of its children.
TEST(DirectoryTreeModelTest, FetchMoreOnALoadedNodeIsANoOp) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->deliver({"bin", "home"});
    ASSERT_EQ(model.rowCount(root), 2);

    model.fetchMore(root);
    model.fetchMore(root);
    EXPECT_EQ(lister->requests.size(), 1); // no repeat listing
    EXPECT_EQ(model.rowCount(root), 2);    // and no duplicated rows
}

// The root's children are rebuilt wholesale when a fallback lands, rather than
// merged with whatever the previous (failed or shallower) level left behind.
TEST(DirectoryTreeModelTest, FallbackReplacesChildrenRatherThanMerging) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({networkRoot("smb://host", "/", {"/share"})},
                   [&](const TreeRoot &) -> TreeDirLister * {
                       lister = new FakeLister;
                       return lister;
                   });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->fail(); // "/" denied -> retry at "/share"
    lister->deliver({"docs", "media"});

    ASSERT_EQ(model.rowCount(root), 2);
    EXPECT_EQ(model.pathAt(model.index(0, 0, root)), QString("/share/docs"));
    EXPECT_EQ(model.pathAt(model.index(1, 0, root)), QString("/share/media"));
}

TEST(DirectoryTreeModelTest, NestedExpansionBuildsCorrectPaths) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->deliver({"home"});

    const QModelIndex home = model.index(0, 0, root);
    ASSERT_TRUE(model.canFetchMore(home));
    model.fetchMore(home);
    EXPECT_EQ(lister->requests.last().path, QString("/home"));
    lister->deliver({"deepin"});

    const QModelIndex user = model.index(0, 0, home);
    EXPECT_EQ(model.pathAt(user), QString("/home/deepin"));
    // The parent chain must round-trip, or the view cannot address the node.
    EXPECT_EQ(model.parent(user), home);
    EXPECT_EQ(model.parent(home), root);
    EXPECT_FALSE(model.parent(root).isValid());
}

// Walking towards a path may only use nodes that already exist: the caller
// expands one level at a time, so a blocking recursive descent (which over a
// network would freeze the UI) is never needed.
TEST(DirectoryTreeModelTest, DeepestLoadedAncestorStopsAtTheMaterialisedEdge) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->deliver({"home"});

    // "/home" exists; "/home/deepin/docs" does not yet.
    const QModelIndex ancestor = model.deepestLoadedAncestor({}, "/home/deepin/docs");
    EXPECT_EQ(model.pathAt(ancestor), QString("/home"));
    EXPECT_FALSE(model.indexForPath({}, "/home/deepin/docs").isValid());
    EXPECT_TRUE(model.indexForPath({}, "/home").isValid());
}

// A path belonging to another connection must not resolve against this root,
// or the tree would select an unrelated directory that happens to share a name.
TEST(DirectoryTreeModelTest, PathLookupIsScopedToItsConnection) {
    DirectoryTreeModel model;
    model.setRoots({localRoot(), networkRoot("smb://host", "/")},
                   [](const TreeRoot &) -> TreeDirLister * { return new FakeLister; });

    EXPECT_TRUE(model.indexForPath({}, "/").isValid());                 // local root
    EXPECT_TRUE(model.indexForPath("smb://host", "/").isValid());       // network root
    EXPECT_FALSE(model.indexForPath("smb://other", "/").isValid());     // unknown connection
}

// "Start from the topmost VISIBLE directory": when the server refuses the
// provider root, the tree drops to the next candidate rather than showing a
// dead root.
TEST(DirectoryTreeModelTest, UnlistableNetworkRootFallsBackToTheNextCandidate) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({networkRoot("smb://host", "/", {"/share", "/share/sub"})},
                   [&](const TreeRoot &) -> TreeDirLister * {
                       lister = new FakeLister;
                       return lister;
                   });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    ASSERT_EQ(lister->requests.size(), 1);
    EXPECT_EQ(lister->requests.at(0).path, QString("/"));

    lister->fail(); // the server denies "/"
    ASSERT_EQ(lister->requests.size(), 2);
    EXPECT_EQ(lister->requests.at(1).path, QString("/share")); // dropped one level

    lister->deliver({"docs"});
    EXPECT_EQ(model.pathAt(root), QString("/share")); // root settled where it can read
    ASSERT_EQ(model.rowCount(root), 1);
    EXPECT_EQ(model.pathAt(model.index(0, 0, root)), QString("/share/docs"));
}

// Once a level lists successfully the remaining candidates are irrelevant; a
// later failure deeper in the tree must not drag the root downwards.
TEST(DirectoryTreeModelTest, SuccessfulRootListingConsumesRemainingFallbacks) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({networkRoot("smb://host", "/", {"/share"})},
                   [&](const TreeRoot &) -> TreeDirLister * {
                       lister = new FakeLister;
                       return lister;
                   });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->deliver({"share", "other"});

    EXPECT_EQ(model.pathAt(root), QString("/"));
    EXPECT_EQ(model.rowCount(root), 2);
    EXPECT_EQ(lister->requests.size(), 1); // no fallback attempt was made
}

// A failed listing with nothing left to try must stop offering expansion, or
// every click would fire another doomed request at the server.
TEST(DirectoryTreeModelTest, FailedListingWithNoFallbacksStopsRetrying) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    lister->fail();

    EXPECT_FALSE(model.canFetchMore(root));
    EXPECT_EQ(model.rowCount(root), 0);
    model.fetchMore(root);
    EXPECT_EQ(lister->requests.size(), 1); // no second attempt
}

// The other panel's connection is visible but inert: not enabled, not
// selectable, and never expandable (which would drive listings on a session
// this panel does not own).
TEST(DirectoryTreeModelTest, NonActivatableRootIsInertAndNeverFetches) {
    TreeRoot root = networkRoot("smb://host", "/");
    root.activatable = false;

    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({root}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });

    const QModelIndex idx = model.index(0, 0);
    EXPECT_FALSE(model.isActivatable(idx));
    EXPECT_EQ(model.flags(idx), Qt::NoItemFlags);
    EXPECT_FALSE(model.hasChildren(idx));
    EXPECT_FALSE(model.canFetchMore(idx));

    model.fetchMore(idx);
    EXPECT_TRUE(lister->requests.isEmpty());
}

// Before a node is fetched the tree must still offer an expander, or lazy
// loading would make every unvisited directory look empty.
TEST(DirectoryTreeModelTest, UnfetchedNodeAdvertisesChildren) {
    DirectoryTreeModel model;
    model.setRoots({localRoot()},
                   [](const TreeRoot &) -> TreeDirLister * { return new FakeLister; });

    EXPECT_TRUE(model.hasChildren(model.index(0, 0)));
}

// Rebuilding the roots (hot-plug, connect, disconnect) must not leave results
// from the old tree resolving into the new one.
TEST(DirectoryTreeModelTest, ResultsForDiscardedRootsAreDropped) {
    FakeLister *lister = nullptr;
    DirectoryTreeModel model;
    model.setRoots({localRoot()}, [&](const TreeRoot &) -> TreeDirLister * {
        lister = new FakeLister;
        return lister;
    });
    // Keep the lister alive past setRoots() so a late result can still be
    // emitted at the model, exactly as an in-flight request would.
    FakeLister survivor;
    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    const quint64 token = lister->requests.first().token;

    // A USB stick appears: the roots are rebuilt and the old nodes are gone.
    model.setRoots({localRoot("/"), localRoot("/media/stick")},
                   [](const TreeRoot &) -> TreeDirLister * { return new FakeLister; });

    // The in-flight result for the discarded node arrives now.
    QSignalSpy spy(&model, &DirectoryTreeModel::childrenLoaded);
    emit survivor.dirsListed(token, "/", {"bin", "home"});

    EXPECT_EQ(spy.count(), 0);
    EXPECT_EQ(model.rowCount(model.index(0, 0)), 0);
    EXPECT_EQ(model.rowCount(), 2);
}
