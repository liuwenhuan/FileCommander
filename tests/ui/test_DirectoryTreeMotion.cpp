#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QTreeView>

#include "FilePanel.h"
#include "MotionPolicy.h"
#include "tree/DirectoryTreeModel.h"
#include "tree/TreeDirLister.h"

namespace {

class MotionPolicyStateGuard {
public:
    MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        MotionPolicy::setApplicationReduced(false);
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        MotionPolicy::setApplicationReduced(false);
    }
};

class TestTreeLister : public TreeDirLister {
public:
    explicit TestTreeLister(int childCount) : m_childCount(childCount) {}

    void requestDirs(quint64 token, const QString &path) override {
        QStringList children;
        if (path == QLatin1String("/root")) {
            children.reserve(m_childCount);
            for (int row = 0; row < m_childCount; ++row)
                children.append(QStringLiteral("branch%1").arg(row));
        } else if (path.startsWith(QLatin1String("/root/branch"))) {
            children.append(QStringLiteral("child"));
        }
        QTimer::singleShot(0, this, [this, token, path, children] {
            emit dirsListed(token, path, children);
        });
    }

    QString childPath(const QString &parent, const QString &name) const override {
        return parent + QLatin1Char('/') + name;
    }

private:
    int m_childCount;
};

QTreeView *directoryTree(FilePanel &panel) {
    for (QTreeView *tree : panel.findChildren<QTreeView *>()) {
        if (qobject_cast<DirectoryTreeModel *>(tree->model())) {
            tree->setVisible(true);
            return tree;
        }
    }
    return nullptr;
}

struct TreeFixture {
    QTreeView *tree = nullptr;
    DirectoryTreeModel *model = nullptr;
    QModelIndex firstBranch;
    QModelIndex secondBranch;
};

TreeFixture prepareTree(FilePanel &panel, int visibleRows) {
    QTreeView *tree = directoryTree(panel);
    EXPECT_NE(tree, nullptr);
    if (!tree)
        return {};

    auto *model = qobject_cast<DirectoryTreeModel *>(tree->model());
    EXPECT_NE(model, nullptr);
    if (!model)
        return {};

    TreeRoot root;
    root.label = QStringLiteral("Root");
    root.basePath = QStringLiteral("/root");
    model->setRoots({root}, [childCount = visibleRows - 1](const TreeRoot &) {
        return new TestTreeLister(childCount);
    });

    const QModelIndex rootIndex = model->index(0, 0);
    tree->setRootIndex(QModelIndex());
    tree->setAnimated(false);
    QSignalSpy childrenLoaded(model, &DirectoryTreeModel::childrenLoaded);
    tree->expand(rootIndex);
    if (childrenLoaded.isEmpty())
        EXPECT_TRUE(childrenLoaded.wait(1000));
    EXPECT_EQ(model->rowCount(rootIndex), visibleRows - 1);
    if (model->rowCount(rootIndex) != visibleRows - 1)
        return {};

    const QModelIndex firstBranch = model->index(0, 0, rootIndex);
    const QModelIndex secondBranch = model->index(1, 0, rootIndex);
    EXPECT_TRUE(firstBranch.isValid());
    EXPECT_TRUE(secondBranch.isValid());
    return {tree, model, firstBranch, secondBranch};
}

QPoint disclosurePoint(const QTreeView &tree, const QModelIndex &index) {
    const QRect row = tree.visualRect(index);
    return {row.left() - tree.indentation() / 2, row.center().y()};
}

TEST(DirectoryTreeMotion, PointerExpansionEnablesAnimationBelowVisibleRowLimit) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);

    fixture.tree->scrollTo(fixture.firstBranch);
    qApp->processEvents();
    QTest::mouseClick(fixture.tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      disclosurePoint(*fixture.tree, fixture.firstBranch));

    QTRY_VERIFY_WITH_TIMEOUT(fixture.tree->isExpanded(fixture.firstBranch), 1000);
    EXPECT_TRUE(fixture.tree->isAnimated());
}

TEST(DirectoryTreeMotion, ExpansionAtVisibleRowLimitDisablesAnimation) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 500);
    ASSERT_NE(fixture.tree, nullptr);

    fixture.tree->scrollTo(fixture.firstBranch);
    qApp->processEvents();
    QTest::mouseClick(fixture.tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      disclosurePoint(*fixture.tree, fixture.firstBranch));

    QTRY_VERIFY_WITH_TIMEOUT(fixture.tree->isExpanded(fixture.firstBranch), 1000);
    EXPECT_FALSE(fixture.tree->isAnimated());
}

TEST(DirectoryTreeMotion, RapidKeyboardExpansionDisablesAnimation) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);

    fixture.tree->setCurrentIndex(fixture.firstBranch);
    QTest::keyClick(fixture.tree, Qt::Key_Right);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.tree->isExpanded(fixture.firstBranch), 1000);
    EXPECT_TRUE(fixture.tree->isAnimated());

    fixture.tree->setCurrentIndex(fixture.secondBranch);
    QTest::keyClick(fixture.tree, Qt::Key_Right);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.tree->isExpanded(fixture.secondBranch), 1000);
    EXPECT_FALSE(fixture.tree->isAnimated());

    QTRY_VERIFY_WITH_TIMEOUT(fixture.tree->isAnimated(), 500);
}

TEST(DirectoryTreeMotion, ReducedMotionExpandsImmediatelyWithoutAnimation) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);

    fixture.tree->setCurrentIndex(fixture.firstBranch);
    QTest::keyClick(fixture.tree, Qt::Key_Right);

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
}

} // namespace
