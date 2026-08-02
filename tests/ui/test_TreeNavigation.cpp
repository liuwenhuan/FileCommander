#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QMenu>

#include <memory>

#include "FilePanel.h"
#include "FileProvider.h"
#include "FileSystemModel.h"
#include "MainWindow.h"
#include "tree/DirectoryTreeModel.h"
#include "tree/NetworkTreeRegistry.h"

// Clicking a local disk in the folder tree while the tab is on a server.
//
// Every root in the tree that is not a connection carries an empty connection
// id, so the panel used to send those paths straight to navigateTo() -- i.e. to
// whatever backend the tab was on. All the backends here are POSIX-rooted, so
// "/home" exists on the share as well as on this machine and the tab quietly
// listed the SERVER's /home while the tree highlighted a local disk. Nothing
// failed and nothing said so.
namespace {

// A server that answers every path with one unmistakable entry, so a listing
// that came from it can be told apart from a real local one at a glance.
class FakeShare : public FileProvider {
public:
    static constexpr const char *kMarker = "SERVER-SIDE-ONLY.txt";

    QString displayName() const override { return QStringLiteral("tester@share"); }
    QString scheme() const override { return QStringLiteral("smb"); }

    QVector<FileInfo> list(const QString &path, bool) const override {
        QVector<FileInfo> out;
        out.append(FileInfo::fromFields(cleanPath(path) + QLatin1Char('/')
                                            + QLatin1String(kMarker),
                                        QLatin1String(kMarker), 7,
                                        QDateTime::currentDateTime(), false, QFile::ReadOwner));
        return out;
    }
    bool isDir(const QString &) const override { return true; }
    QString cleanPath(const QString &path) const override {
        QString p = path;
        while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
            p.chop(1);
        return p;
    }
    QString parentPath(const QString &path) const override {
        const QString clean = cleanPath(path);
        const int slash = clean.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        return slash == 0 ? QStringLiteral("/") : clean.left(slash);
    }
    bool exists(const QString &) const override { return true; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

void settle(FilePanel &panel) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    if (spy.isEmpty())
        spy.wait(4000);
    QCoreApplication::processEvents();
}

QTreeView *treeOf(FilePanel &panel) {
    for (QTreeView *t : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(t->model()))
            return t;
    if (auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton")))
        button->click();
    for (QTreeView *t : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(t->model()))
            return t;
    return nullptr;
}

// The first top-level tree row that stands for a real local directory.
QModelIndex firstLocalRoot(DirectoryTreeModel *model) {
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (!model->connectionIdAt(idx).isEmpty() || !model->isActivatable(idx))
            continue;
        const QString path = model->pathAt(idx);
        if (!path.isEmpty() && QFileInfo(path).isDir())
            return idx;
    }
    return {};
}

// Activating a tree row is a signal the panel listens to; invoking it through
// the meta-object is the same thing a double-click or Enter produces.
void activate(QTreeView *tree, const QModelIndex &index) {
    QMetaObject::invokeMethod(tree, "activated", Q_ARG(QModelIndex, index));
}

bool listHasName(FileSystemModel *model, const QString &name) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->fileInfoAt(row).name() == name)
            return true;
    return false;
}

} // namespace

TEST(TreeNavigationTest, LocalDiskOnANetworkTabListsTheLocalDiskNotTheServer) {
    FilePanel panel;
    panel.connectTabTo(0, std::make_shared<FakeShare>(), [](QString *) { return true; },
                       QStringLiteral("/home"), QStringLiteral("tester@share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(panel);
    ASSERT_TRUE(panel.model()->hasNetworkSession());
    ASSERT_TRUE(listHasName(panel.model(), QLatin1String(FakeShare::kMarker)))
        << "the tab is not actually showing the fake server";

    QTreeView *tree = treeOf(panel);
    ASSERT_NE(tree, nullptr);
    panel.rebuildTreeRoots();
    auto *treeModel = qobject_cast<DirectoryTreeModel *>(tree->model());
    const QModelIndex localRoot = firstLocalRoot(treeModel);
    ASSERT_TRUE(localRoot.isValid()) << "no local disk in the tree to click";
    const QString localPath = treeModel->pathAt(localRoot);

    activate(tree, localRoot);
    settle(panel);

    EXPECT_EQ(panel.currentPath(), localPath);
    // The three things that were wrong: the listing came off the server, the
    // backend was still the server's, and the tab still claimed its connection.
    EXPECT_FALSE(listHasName(panel.model(), QLatin1String(FakeShare::kMarker)))
        << "listed the server's " << localPath.toStdString() << ", not this machine's";
    ASSERT_NE(panel.model()->provider(), nullptr);
    EXPECT_TRUE(panel.model()->provider()->isLocalFilesystem());
    EXPECT_TRUE(panel.connectionId().isEmpty());
}

TEST(TreeNavigationTest, TheServerStaysReachableWithOneBackPress) {
    // Going local must not throw the connection away -- the tree click is a
    // navigation, not a disconnect, so Back has to return to the share.
    FilePanel panel;
    panel.connectTabTo(0, std::make_shared<FakeShare>(), [](QString *) { return true; },
                       QStringLiteral("/home"), QStringLiteral("tester@share"),
                       SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(panel);
    const QString connId = panel.connectionId();
    ASSERT_FALSE(connId.isEmpty());

    QTreeView *tree = treeOf(panel);
    ASSERT_NE(tree, nullptr);
    panel.rebuildTreeRoots();
    auto *treeModel = qobject_cast<DirectoryTreeModel *>(tree->model());
    const QModelIndex localRoot = firstLocalRoot(treeModel);
    ASSERT_TRUE(localRoot.isValid());

    activate(tree, localRoot);
    settle(panel);
    ASSERT_TRUE(panel.connectionId().isEmpty());

    panel.goBack();
    settle(panel);
    EXPECT_EQ(panel.connectionId(), connId);
    EXPECT_EQ(panel.currentPath().toStdString(), "/home");
    EXPECT_TRUE(listHasName(panel.model(), QLatin1String(FakeShare::kMarker)));
}

TEST(TreeNavigationTest, LocalTabStillNavigatesStraightToTheClickedFolder) {
    // The regression guard: on an ordinary local tab a tree click is a plain
    // navigation and must stay one -- same directory, same local backend, and
    // the previous directory on the Back stack.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("alpha")));
    const QString alpha = QDir(dir.path()).filePath(QStringLiteral("alpha"));

    FilePanel panel;
    panel.navigateTo(dir.path());
    settle(panel);

    QTreeView *tree = treeOf(panel);
    ASSERT_NE(tree, nullptr);
    panel.rebuildTreeRoots();
    auto *treeModel = qobject_cast<DirectoryTreeModel *>(tree->model());
    const QModelIndex localRoot = firstLocalRoot(treeModel);
    ASSERT_TRUE(localRoot.isValid());
    const QString localPath = treeModel->pathAt(localRoot);

    activate(tree, localRoot);
    settle(panel);
    EXPECT_EQ(panel.currentPath(), localPath);
    ASSERT_NE(panel.model()->provider(), nullptr);
    EXPECT_TRUE(panel.model()->provider()->isLocalFilesystem());

    panel.goBack();
    settle(panel);
    EXPECT_EQ(panel.currentPath(), dir.path());

    // And a direct navigation into a sub-folder still works afterwards.
    panel.navigateTo(alpha);
    settle(panel);
    EXPECT_EQ(panel.currentPath(), alpha);
}

// Two settings have been retired: "Reduce Motion" (motion is no longer
// user-configurable) and "Associate Folder Open Actions" (registering the app as
// the system's folder handler). Neither may come back through the config menu.
TEST(MainWindowTest, ConfigMenuOmitsRetiredActions) {
    MainWindow window;
    QMenu *configMenu = nullptr;
    for (QMenu *menu : window.findChildren<QMenu *>()) {
        if (menu->title() == QStringLiteral("Con&fig")) {
            configMenu = menu;
            break;
        }
    }
    ASSERT_NE(configMenu, nullptr);

    // The menu fills itself on first show, so wait for an entry that is expected
    // to survive before asserting on the ones that must not be there.
    configMenu->popup(QPoint(10, 10));
    QTRY_VERIFY_WITH_TIMEOUT(
        configMenu->findChild<QAction *>(QStringLiteral("configAutoUpdateAction")) != nullptr, 500);
    configMenu->hide();

    ASSERT_FALSE(configMenu->actions().isEmpty());
    EXPECT_EQ(configMenu->findChild<QAction *>(QStringLiteral("configFolderAssociationAction")),
              nullptr);
    EXPECT_EQ(configMenu->findChild<QAction *>(QStringLiteral("configReduceMotionAction")), nullptr);
    for (QAction *action : configMenu->actions()) {
        EXPECT_NE(action->text(), QStringLiteral("Reduce Motion"));
        EXPECT_NE(action->text(), QStringLiteral("Associate Folder Open Actions"));
    }
}

#if defined(Q_OS_WIN) && FILECOMMANDER_HAS_NETWORK
TEST(MainWindowNetworkTest, ConnectedServerAppearsInTheFolderTree) {
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<NetworkTreeRegistry *>() != nullptr, 6000);
    const QList<FilePanel *> panels = window.findChildren<FilePanel *>();
    ASSERT_GE(panels.size(), 2);

    FilePanel *panel = panels.first();
    panel->connectTabTo(0, std::make_shared<FakeShare>(), [](QString *) { return true; },
                        QStringLiteral("/"), QStringLiteral("tester@share"),
                        SavedConnection{}, FileSystemModel::AuthRetryFactory());
    settle(*panel);
    const QString connectionId = panel->connectionId();
    ASSERT_FALSE(connectionId.isEmpty());

    panel->rebuildTreeRoots();
    QTreeView *tree = treeOf(*panel);
    ASSERT_NE(tree, nullptr);
    auto *treeModel = qobject_cast<DirectoryTreeModel *>(tree->model());
    ASSERT_NE(treeModel, nullptr);

    bool foundConnectionRoot = false;
    for (int row = 0; row < treeModel->rowCount(); ++row) {
        if (treeModel->connectionIdAt(treeModel->index(row, 0)) == connectionId) {
            foundConnectionRoot = true;
            break;
        }
    }
    EXPECT_TRUE(foundConnectionRoot);
}

#endif
