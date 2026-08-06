#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QApplication>
#include <QDir>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QElapsedTimer>
#include <QTreeView>
#include <QVariantAnimation>

#include <memory>

#include "FilePanel.h"
#include "FileSystemModel.h"
#include "MotionPolicy.h"
#include "tree/DirectoryTreeModel.h"
#include "tree/TreeDirLister.h"

namespace {

constexpr auto kFeedbackAnimationName = "DirectoryTreeDisclosureFeedbackAnimation";

class MotionPolicyStateGuard {
public:
    MotionPolicyStateGuard() : m_disableAnimations(qgetenv("FILECOMMANDER_DISABLE_ANIMATIONS")) {
        qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", "0");
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
        if (m_disableAnimations.isNull())
            qunsetenv("FILECOMMANDER_DISABLE_ANIMATIONS");
        else
            qputenv("FILECOMMANDER_DISABLE_ANIMATIONS", m_disableAnimations);
    }

private:
    QByteArray m_disableAnimations;
};

class TestTreeLister : public TreeDirLister {
public:
    TestTreeLister(int childCount, QString rootPath)
        : m_childCount(childCount), m_rootPath(QDir::cleanPath(std::move(rootPath))) {}

    void requestDirs(quint64 token, const QString &path) override {
        QStringList children;
        if (QDir::cleanPath(path) == m_rootPath) {
            children.reserve(m_childCount);
            for (int row = 0; row < m_childCount; ++row)
                children.append(QStringLiteral("branch%1").arg(row));
        } else if (QDir::cleanPath(path).startsWith(m_rootPath + QLatin1Char('/'))) {
            children.append(QStringLiteral("child"));
        }
        QTimer::singleShot(0, this, [this, token, path, children] {
            emit dirsListed(token, path, children);
        });
    }

    QString childPath(const QString &parent, const QString &name) const override {
        return QDir(parent).filePath(name);
    }

private:
    int m_childCount;
    QString m_rootPath;
};

QTreeView *directoryTree(FilePanel &panel) {
    for (QTreeView *tree : panel.findChildren<QTreeView *>()) {
        if (qobject_cast<DirectoryTreeModel *>(tree->model())) {
            tree->setVisible(true);
            return tree;
        }
    }
    if (auto *button = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton")))
        button->click();
    for (QTreeView *tree : panel.findChildren<QTreeView *>())
        if (qobject_cast<DirectoryTreeModel *>(tree->model()))
            return tree;
    return nullptr;
}

QVariantAnimation *feedbackAnimation(QTreeView *tree) {
    return tree ? tree->findChild<QVariantAnimation *>(
                      QString::fromLatin1(kFeedbackAnimationName))
                : nullptr;
}

struct TreeFixture {
    QTreeView *tree = nullptr;
    DirectoryTreeModel *model = nullptr;
    QModelIndex root;
    QModelIndex firstBranch;
    QModelIndex secondBranch;
};

TreeFixture prepareTree(FilePanel &panel, int visibleRows,
                        const QString &rootPath = QStringLiteral("/root")) {
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
    root.basePath = rootPath;
    model->setRoots({root}, [childCount = visibleRows - 1, rootPath](const TreeRoot &) {
        return new TestTreeLister(childCount, rootPath);
    });

    const QModelIndex rootIndex = model->index(0, 0);
    tree->setRootIndex(QModelIndex());
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
    return {tree, model, rootIndex, firstBranch, secondBranch};
}

QPoint disclosurePoint(const QTreeView &tree, const QModelIndex &index) {
    const QRect row = tree.visualRect(index);
    return {row.left() - tree.indentation() / 2, row.center().y()};
}

void clickDisclosure(QTreeView *tree, const QModelIndex &index) {
    tree->scrollTo(index);
    qApp->processEvents();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      disclosurePoint(*tree, index));
}

void activate(QTreeView *tree, const QModelIndex &index) {
    QMetaObject::invokeMethod(tree, "activated", Q_ARG(QModelIndex, index));
}

TEST(DirectoryTreeMotion, PointerFeedbackIsPaintOnlyAndUsesMotionPolicyTiming) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);

    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);
    clickDisclosure(fixture.tree, fixture.firstBranch);

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->duration(), 100);
    EXPECT_EQ(feedback->easingCurve().type(), QEasingCurve::OutCubic);
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Running);
    EXPECT_GT(feedback->currentValue().toReal(), 0.0);
}

TEST(DirectoryTreeMotion, VisibleRowLimitSkipsFeedbackAtFiveHundredRows) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 500);
    ASSERT_NE(fixture.tree, nullptr);

    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);
    clickDisclosure(fixture.tree, fixture.firstBranch);

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Stopped);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);
}

TEST(DirectoryTreeMotion, RapidPointerTogglesShareOneCadenceWindow) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);

    clickDisclosure(fixture.tree, fixture.firstBranch);
    ASSERT_EQ(feedback->state(), QAbstractAnimation::Running);
    clickDisclosure(fixture.tree, fixture.firstBranch);

    EXPECT_FALSE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Stopped);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);
}

TEST(DirectoryTreeMotion, MixedKeyboardAndPointerTogglesShareOneCadenceWindow) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);

    fixture.tree->setCurrentIndex(fixture.firstBranch);
    QTest::keyClick(fixture.tree, Qt::Key_Right);
    ASSERT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    ASSERT_EQ(feedback->state(), QAbstractAnimation::Running);

    clickDisclosure(fixture.tree, fixture.secondBranch);

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.secondBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Stopped);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);
}

TEST(DirectoryTreeMotion, ProgrammaticExpansionAndAsyncChildrenDoNotStartFeedback) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);
    QSignalSpy stateChanges(feedback, &QVariantAnimation::stateChanged);
    QSignalSpy childrenLoaded(fixture.model, &DirectoryTreeModel::childrenLoaded);

    fixture.tree->expand(fixture.firstBranch);
    if (childrenLoaded.isEmpty())
        ASSERT_TRUE(childrenLoaded.wait(1000));

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_EQ(fixture.model->rowCount(fixture.firstBranch), 1);
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Stopped);
    EXPECT_EQ(stateChanges.count(), 0);
}

TEST(DirectoryTreeMotion, LiveReducedMotionFinishesPendingFeedbackSynchronously) {
    MotionPolicyStateGuard guard;
    MotionPolicy::clearReducedForTest();
    MotionPolicy::setSystemReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);

    clickDisclosure(fixture.tree, fixture.firstBranch);
    ASSERT_EQ(feedback->state(), QAbstractAnimation::Running);
    ASSERT_GT(feedback->currentValue().toReal(), 0.0);

    MotionPolicy::setSystemReducedForTest(true);

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Stopped);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);
}

TEST(DirectoryTreeMotion, FeedbackPreservesSelectionScrollAndSettledRowGeometry) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);

    const QModelIndex target = fixture.model->index(60, 0, fixture.root);
    const QModelIndex selected = fixture.model->index(61, 0, fixture.root);
    ASSERT_TRUE(target.isValid());
    ASSERT_TRUE(selected.isValid());
    fixture.tree->scrollTo(target, QAbstractItemView::PositionAtCenter);
    fixture.tree->setCurrentIndex(selected);
    fixture.tree->selectionModel()->select(selected, QItemSelectionModel::ClearAndSelect);
    qApp->processEvents();
    const QPersistentModelIndex expectedCurrent(selected);

    QSignalSpy childrenLoaded(fixture.model, &DirectoryTreeModel::childrenLoaded);
    QTest::mouseClick(fixture.tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      disclosurePoint(*fixture.tree, target));
    if (childrenLoaded.isEmpty())
        ASSERT_TRUE(childrenLoaded.wait(1000));
    qApp->processEvents();

    const int settledScroll = fixture.tree->verticalScrollBar()->value();
    const QRect settledTargetRect = fixture.tree->visualRect(target);
    const QRect settledSelectedRect = fixture.tree->visualRect(selected);
    const QSize settledSizeHint = fixture.tree->sizeHint();
    QTest::qWait(45);

    EXPECT_EQ(fixture.tree->currentIndex(), QModelIndex(expectedCurrent));
    EXPECT_TRUE(fixture.tree->selectionModel()->isSelected(expectedCurrent));
    EXPECT_EQ(fixture.tree->verticalScrollBar()->value(), settledScroll);
    EXPECT_EQ(fixture.tree->visualRect(target), settledTargetRect);
    EXPECT_EQ(fixture.tree->visualRect(selected), settledSelectedRect);
    EXPECT_EQ(fixture.tree->sizeHint(), settledSizeHint);
    EXPECT_FALSE(fixture.tree->isAnimated());
}

TEST(DirectoryTreeMotion, NavigationRemainsAvailableWhileFeedbackIsRunning) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(QDir(temporary.path()).mkpath(QStringLiteral("branch0")));
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("branch0"));

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 3, temporary.path());
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);

    clickDisclosure(fixture.tree, fixture.firstBranch);
    ASSERT_EQ(feedback->state(), QAbstractAnimation::Running);
    QSignalSpy loaded(panel.model(), &FileSystemModel::loadFinished);
    activate(fixture.tree, fixture.firstBranch);
    if (loaded.isEmpty())
        ASSERT_TRUE(loaded.wait(4000));

    EXPECT_EQ(panel.currentPath(), destination);
    EXPECT_FALSE(fixture.tree->isAnimated());
}

TEST(DirectoryTreeMotion, PendingFeedbackIsSafeDuringTeardown) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);
    QPointer<QVariantAnimation> feedback;

    {
        auto panel = std::make_unique<FilePanel>();
        panel->resize(700, 500);
        panel->show();
        qApp->processEvents();
        const TreeFixture fixture = prepareTree(*panel, 100);
        ASSERT_NE(fixture.tree, nullptr);
        feedback = feedbackAnimation(fixture.tree);
        ASSERT_FALSE(feedback.isNull());

        clickDisclosure(fixture.tree, fixture.firstBranch);
        ASSERT_EQ(feedback->state(), QAbstractAnimation::Running);
    }

    EXPECT_TRUE(feedback.isNull());
    MotionPolicy::setReducedForTest(true);
    QTest::qWait(125);
    EXPECT_TRUE(feedback.isNull());
}

// Waits until the tree will treat the next click as fresh input rather than as
// part of a burst.
//
// Waiting for the feedback ANIMATION to stop is not the same thing and is what
// made this flaky: the cadence window and the animation are both 100 ms and
// start together, so a click placed just after the animation settles lands on
// the boundary and is rapid or not depending on which timer the event loop
// serviced first.
void waitForFreshInput(QTreeView *tree) {
    auto *cadence = tree->findChild<QTimer *>(QStringLiteral("TreeInputCadenceTimer"));
    ASSERT_NE(cadence, nullptr) << "the tree publishes no cadence timer to wait on";
    QElapsedTimer budget;
    budget.start();
    while (cadence->isActive() && budget.elapsed() < 2000)
        QTest::qWait(10);
    ASSERT_FALSE(cadence->isActive()) << "the cadence window never lapsed";
}

TEST(DirectoryTreeMotion, RepeatedExpandCollapseFeedbackSettlesAndRestarts) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);

    // Counted, not caught in the act.
    //
    // This used to assert the animation was still Running the instant the click
    // returned -- that it had not finished YET. Whether it has is a race with
    // the machine: the feedback is short, and on a busy run it can be over
    // before the next line executes. Waiting longer cannot fix an assertion
    // that something has not happened yet.
    //
    // A START, though, is a fact. It emits stateChanged(Running, Stopped) when
    // it happens and stays counted afterwards, so the same property -- every
    // click restarts the feedback -- is exact however the timing falls.
    int starts = 0;
    QObject::connect(feedback, &QAbstractAnimation::stateChanged, feedback,
                     [&starts](QAbstractAnimation::State to, QAbstractAnimation::State) {
                         if (to == QAbstractAnimation::Running)
                             ++starts;
                     });

    clickDisclosure(fixture.tree, fixture.firstBranch);
    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_EQ(starts, 1) << "expanding did not start the feedback";
    FC_TRY_COMPARE_WITH_TIMEOUT(feedback->state(), QAbstractAnimation::Stopped, 2000);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);

    waitForFreshInput(fixture.tree);
    clickDisclosure(fixture.tree, fixture.firstBranch);
    EXPECT_FALSE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_EQ(starts, 2) << "collapsing did not restart the settled feedback";
    FC_TRY_COMPARE_WITH_TIMEOUT(feedback->state(), QAbstractAnimation::Stopped, 2000);

    waitForFreshInput(fixture.tree);
    clickDisclosure(fixture.tree, fixture.firstBranch);
    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_EQ(starts, 3) << "expanding again did not restart the settled feedback";
    FC_TRY_COMPARE_WITH_TIMEOUT(feedback->state(), QAbstractAnimation::Stopped, 2000);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);
    EXPECT_FALSE(fixture.tree->isAnimated());
}

TEST(DirectoryTreeMotion, ReducedMotionExpandsImmediatelyWithoutFeedback) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();
    const TreeFixture fixture = prepareTree(panel, 100);
    ASSERT_NE(fixture.tree, nullptr);
    QVariantAnimation *feedback = feedbackAnimation(fixture.tree);
    ASSERT_NE(feedback, nullptr);

    fixture.tree->setCurrentIndex(fixture.firstBranch);
    QTest::keyClick(fixture.tree, Qt::Key_Right);

    EXPECT_TRUE(fixture.tree->isExpanded(fixture.firstBranch));
    EXPECT_FALSE(fixture.tree->isAnimated());
    EXPECT_EQ(feedback->state(), QAbstractAnimation::Stopped);
    EXPECT_DOUBLE_EQ(feedback->currentValue().toReal(), 0.0);
}

} // namespace
