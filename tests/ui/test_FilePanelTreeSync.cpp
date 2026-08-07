#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

#include <QElapsedTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "FilePanel.h"
#include "config/Settings.h"
#include "FileListView.h"
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
QTreeView *directoryTree(FilePanel &panel) {
    for (QTreeView *t : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(t->model()))
            return t;
    return nullptr;
}

QTreeView *showTree(FilePanel &panel) {
    QTreeView *tree = directoryTree(panel);
    if (tree) {
        tree->setVisible(true);
    } else {
        auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton"));
        if (!button)
            return nullptr;
        button->click();
        tree = directoryTree(panel);
    }

    // The fixture sits under the system temp directory, and on Windows the only
    // route there runs through AppData, which is hidden. A tree that filters
    // hidden directories cannot reach it -- it settles on C:/Users/<user>, by
    // design, and that is exactly what Windows CI reported. So ask for hidden
    // directories, which the tree reads when its roots are rebuilt.
    //
    // Through toggleHiddenFiles rather than the model's setter directly: that
    // is the path a user takes, and until recently it left the tree behind.
    panel.toggleHiddenFiles();
    panel.rebuildTreeRoots();
    return tree;
}

// The path the tree's current row stands for, or empty when nothing is current.
QString treeCurrentPath(QTreeView *tree) {
    auto *model = qobject_cast<DirectoryTreeModel *>(tree->model());
    const QModelIndex cur = tree->currentIndex();
    return cur.isValid() ? model->pathAt(cur) : QString();
}

// Says where a tree walk lost the trail. This exists because the walk fails on
// Windows CI and nowhere else, always stopping at exactly C:/Users/runneradmin.
//
// Two explanations have already been tried and disproved by measurement -- that
// the walk was too slow (widening the budget to 15s changed nothing), and that
// a hidden AppData blocked it (hiding AppData locally did not reproduce it).
// Each guess cost a CI round trip. So instead of a third guess, a failure now
// reports what the tree actually holds at the point it gave up: which ancestor
// it reached, that node's children, and whether any of them leads onward. That
// separates "not listed yet" from "listed but filtered out" from "listed under
// a different name" -- which is as far as reasoning from here can get.
QString describeTreeWalk(QTreeView *tree, const QString &target) {
    auto *model = qobject_cast<DirectoryTreeModel *>(tree->model());
    if (!model)
        return QStringLiteral("no DirectoryTreeModel on the view");

    QString out = QStringLiteral("\n  target : %1\n  current: %2\n")
                      .arg(target, treeCurrentPath(tree));

    // Walk down from the roots, following the target one component at a time.
    QModelIndex node;
    for (int depth = 0; depth < 24; ++depth) {
        const int rows = model->rowCount(node);
        QStringList children;
        QModelIndex next;
        for (int r = 0; r < rows; ++r) {
            const QModelIndex child = model->index(r, 0, node);
            const QString path = model->pathAt(child);
            children << path;
            // The one child that is the target, or an ancestor of it. A drive
            // root already ends in '/', so appending another separator would
            // never match and the walk would report giving up at depth 0.
            const QString prefix =
                path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/');
            if (target == path || target.startsWith(prefix))
                next = child;
        }
        const QString here =
            node.isValid() ? model->pathAt(node) : QStringLiteral("<roots>");
        // The component the walk needs next -- naming it turns the dump into an
        // answer rather than a list to read. A directory with a thousand
        // entries would otherwise bury it, and the interesting level (a user
        // profile) is the one where a single missing name decides everything.
        const QString wanted =
            here == QStringLiteral("<roots>")
                ? QString()
                : target.mid(here.size()).split(QLatin1Char('/'), QString::SkipEmptyParts).value(0);
        out += QStringLiteral("  depth %1 under '%2': %3 children, wants '%4' -> %5\n")
                   .arg(depth)
                   .arg(here)
                   .arg(rows)
                   .arg(wanted,
                        next.isValid() ? QStringLiteral("found") : QStringLiteral("MISSING"));
        // Names too, but few: enough to tell an empty listing from a filtered
        // one without flooding a CI log.
        if (!children.isEmpty())
            out += QStringLiteral("      %1%2\n")
                       .arg(QStringList(children.mid(0, 12)).join(QStringLiteral(", ")),
                            children.size() > 12
                                ? QStringLiteral(" ... (+%1)").arg(children.size() - 12)
                                : QString());
        if (!next.isValid()) {
            out += QStringLiteral("      -> the walk stops here. canFetchMore=%1, expanded=%2\n")
                       .arg(model->canFetchMore(node) ? QStringLiteral("yes") : QStringLiteral("no"),
                            tree->isExpanded(node) ? QStringLiteral("yes") : QStringLiteral("no"));
            break;
        }
        if (model->pathAt(next) == target) {
            out += QStringLiteral("      -> the target node exists in the tree; "
                                  "it simply was never made current\n");
            break;
        }
        node = next;
    }
    return out;
}

// Same wait as FC_TRY_COMPARE_WITH_TIMEOUT, but a failure prints where the walk
// stopped instead of only the two paths that differ.
//
// Compared as std::string: gtest has no printer for QString and falls back to
// dumping it as a list of two-byte objects, which buries the one line of the
// failure anybody reads.
#define FC_TRY_TREE_PATH(tree, expected, timeoutMs)                                                \
    do {                                                                                           \
        const QString fcTreeWanted = (expected);                                                   \
        QElapsedTimer fcTreeTimer;                                                                 \
        fcTreeTimer.start();                                                                       \
        while (treeCurrentPath(tree) != fcTreeWanted && fcTreeTimer.elapsed() < (timeoutMs))       \
            QTest::qWait(20);                                                                      \
        ASSERT_EQ(treeCurrentPath(tree).toStdString(), fcTreeWanted.toStdString())                 \
            << qPrintable(describeTreeWalk(tree, fcTreeWanted));                                   \
    } while (false)

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
    QTemporaryDir m_dir{
        QDir(QDir::tempPath()).filePath(QStringLiteral("ttc-treesync-XXXXXX"))};
    QString m_alpha;
    QString m_alphaInner;
    QString m_beta;
};

// The waits below are generous on purpose, though slowness was never the cause
// -- widening them from 4s to 15s changed nothing. They are wide because a
// budget on "wait until X" only bounds how long a real failure takes to report:
// a generous one costs nothing when the test passes, and a tight one fails on a
// slow machine for no reason.
TEST_F(FilePanelTreeSyncTest, TreeFollowsTabSwitchAfterEachLoad) {
    FilePanel panel;
    panel.show();
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);

    ASSERT_TRUE(navigateAndWait(panel, m_alphaInner));
    FC_TRY_TREE_PATH(tree, m_alphaInner, 15000);

    panel.newTab();
    ASSERT_TRUE(navigateAndWait(panel, m_beta));
    FC_TRY_TREE_PATH(tree, m_beta, 15000);

    QSignalSpy previousLoad(panel.model(), &FileSystemModel::loadFinished);
    panel.prevTab();
    ASSERT_TRUE(waitForLoad(previousLoad));
    FC_TRY_TREE_PATH(tree, m_alphaInner, 15000);

    QSignalSpy nextLoad(panel.model(), &FileSystemModel::loadFinished);
    panel.nextTab();
    ASSERT_TRUE(waitForLoad(nextLoad));
    FC_TRY_TREE_PATH(tree, m_beta, 15000);
}

TEST(FilePanelStartupTest, DirectoryTreeIsCreatedOnceOnFirstToggleWithCurrentFont) {
    FilePanel panel;
    panel.setListFontFamily(QStringLiteral("Courier New"));
    panel.setListFontSize(17);

    EXPECT_EQ(directoryTree(panel), nullptr);
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->font().family(), panel.view()->font().family());
    EXPECT_EQ(tree->font().pointSize(), 17);
    EXPECT_EQ(tree->viewport()->font().pointSize(), 17);

    auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton"));
    ASSERT_NE(button, nullptr);
    button->click();
    button->click();
    EXPECT_EQ(directoryTree(panel), tree);
}

TEST(FilePanelStartupTest, DirectoryTreeBuildsRootsWhenFirstShown) {
    FilePanel panel;
    EXPECT_EQ(directoryTree(panel), nullptr);
    panel.show();
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<DirectoryTreeModel *>(tree->model());
    ASSERT_NE(model, nullptr);

    FC_TRY_VERIFY_WITH_TIMEOUT(model->rowCount() > 0, 5000);
}

TEST_F(FilePanelTreeSyncTest, HiddenTreePreservesItsExistingSelectionUntilReopened) {
    FilePanel panel;
    panel.show();
    QTreeView *tree = showTree(panel);
    ASSERT_NE(tree, nullptr);

    ASSERT_TRUE(navigateAndWait(panel, m_alpha));
    FC_TRY_TREE_PATH(tree, m_alpha, 15000);
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
    FC_TRY_TREE_PATH(tree, m_beta, 15000);
}

// Turning on hidden files has to take the tree with it.
//
// It did not, and the gap was invisible here for a Windows-specific reason: the
// tree reads the flag once, when its roots are rebuilt, and toggleHiddenFiles
// rebuilt nothing. The file list would walk into a hidden directory while the
// tree stayed outside it, settling on the last visible ancestor. On Windows
// that is every path under AppData -- which includes the whole temp directory,
// and so every fixture in this file.
//
// Deliberately does NOT call rebuildTreeRoots: the other tests here do, so they
// would keep passing with the fix reverted. This one only toggles.
TEST_F(FilePanelTreeSyncTest, TurningOnHiddenFilesLetsTheTreeFollowIntoAHiddenDirectory) {
    const QDir root(m_dir.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral(".secret/leaf")));
    const QString leaf = root.filePath(QStringLiteral(".secret/leaf"));
#ifdef Q_OS_WIN
    // A leading dot means nothing to Windows; the attribute is what hides it.
    ASSERT_TRUE(SetFileAttributesW(
        reinterpret_cast<const wchar_t *>(root.filePath(QStringLiteral(".secret")).utf16()),
        FILE_ATTRIBUTE_HIDDEN));
#endif

    FilePanel panel;
    panel.show();
    // Not showTree(): that helper turns hidden files on, which is the very
    // thing under test.
    QTreeView *tree = directoryTree(panel);
    if (tree) {
        tree->setVisible(true);
    } else {
        auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton"));
        ASSERT_NE(button, nullptr);
        button->click();
        tree = directoryTree(panel);
    }
    ASSERT_NE(tree, nullptr);

    // With hidden files off the tree cannot reach the leaf, and settling short
    // of it is correct -- so this asserts only that it did not somehow arrive.
    ASSERT_TRUE(navigateAndWait(panel, leaf));
    QTest::qWait(500);
    ASSERT_NE(treeCurrentPath(tree), leaf) << "the fixture is not hidden after all, so this "
                                              "test cannot tell the fix from its absence";

    panel.toggleHiddenFiles();
    FC_TRY_TREE_PATH(tree, leaf, 15000);
}

} // namespace
