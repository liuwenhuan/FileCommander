#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include "FilePanel.h"
#include "FileSystemModel.h"
#include "tree/DirectoryTreeModel.h"

// Switching tabs has to move the folder tree's selection to the incoming tab's
// directory. The walk itself (syncTreeToPath) was always right; it simply was
// never called on a tab switch, so the tree stayed on the outgoing tab's
// directory until some later reload happened to re-trigger it.
namespace {

// Waits for the panel's asynchronous directory load to land.
bool navigateAndWait(FilePanel &panel, const QString &path) {
    QSignalSpy spy(panel.model(), &FileSystemModel::loadFinished);
    panel.navigateTo(path);
    return !spy.isEmpty() || spy.wait(4000);
}

bool waitForLoad(QSignalSpy &spy) {
    return !spy.isEmpty() || spy.wait(4000);
}

// The tree has no accessor on FilePanel, so find it by its model type: it is
// the panel's only QTreeView over a DirectoryTreeModel.
QTreeView *showTree(FilePanel &panel) {
    for (QTreeView *t : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(t->model())) {
            t->setVisible(true);
            return t;
        }
    return nullptr;
}

// The path the tree's current row stands for, or empty when nothing is current.
QString treeCurrentPath(QTreeView *tree) {
    auto *model = qobject_cast<DirectoryTreeModel *>(tree->model());
    const QModelIndex cur = tree->currentIndex();
    return cur.isValid() ? model->pathAt(cur) : QString();
}

class FilePanelTreeSyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        const QDir root(m_dir.path());
        ASSERT_TRUE(root.mkpath(QStringLiteral("alpha/inner")));
        ASSERT_TRUE(root.mkpath(QStringLiteral("beta")));
        m_alpha = root.filePath(QStringLiteral("alpha"));
        m_alphaInner = root.filePath(QStringLiteral("alpha/inner"));
        m_beta = root.filePath(QStringLiteral("beta"));
    }
    QTemporaryDir m_dir{QStringLiteral("/tmp/ttc-treesync-XXXXXX")};
    QString m_alpha;
    QString m_alphaInner;
    QString m_beta;
};

TEST_F(FilePanelTreeSyncTest, TreeFollowsTabSwitchAfterEachLoad) {
    FilePanel panel;
    panel.show();
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);

    ASSERT_TRUE(navigateAndWait(panel, m_alphaInner));
    QTRY_COMPARE_WITH_TIMEOUT(treeCurrentPath(tree), m_alphaInner, 4000);

    panel.newTab();
    ASSERT_TRUE(navigateAndWait(panel, m_beta));
    QTRY_COMPARE_WITH_TIMEOUT(treeCurrentPath(tree), m_beta, 4000);

    QSignalSpy previousLoad(panel.model(), &FileSystemModel::loadFinished);
    panel.prevTab();
    ASSERT_TRUE(waitForLoad(previousLoad));
    QTRY_COMPARE_WITH_TIMEOUT(treeCurrentPath(tree), m_alphaInner, 4000);

    QSignalSpy nextLoad(panel.model(), &FileSystemModel::loadFinished);
    panel.nextTab();
    ASSERT_TRUE(waitForLoad(nextLoad));
    QTRY_COMPARE_WITH_TIMEOUT(treeCurrentPath(tree), m_beta, 4000);
}

TEST_F(FilePanelTreeSyncTest, HiddenTreePreservesItsExistingSelectionUntilReopened) {
    FilePanel panel;
    panel.show();
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);

    ASSERT_TRUE(navigateAndWait(panel, m_alpha));
    QTRY_COMPARE_WITH_TIMEOUT(treeCurrentPath(tree), m_alpha, 4000);
    tree->setVisible(false);

    panel.newTab();
    ASSERT_TRUE(navigateAndWait(panel, m_beta));
    EXPECT_EQ(treeCurrentPath(tree), m_alpha);

    QSignalSpy previousLoad(panel.model(), &FileSystemModel::loadFinished);
    panel.prevTab();
    ASSERT_TRUE(waitForLoad(previousLoad));
    EXPECT_EQ(treeCurrentPath(tree), m_alpha);

    tree->setVisible(true);
    QSignalSpy nextLoad(panel.model(), &FileSystemModel::loadFinished);
    panel.nextTab();
    ASSERT_TRUE(waitForLoad(nextLoad));
    QTRY_COMPARE_WITH_TIMEOUT(treeCurrentPath(tree), m_beta, 4000);
}

} // namespace
