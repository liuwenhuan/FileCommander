#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QSplitter>
#include <QTest>
#include <QToolButton>
#include <QVariant>

#include "FilePanel.h"
#include "FileListView.h"
#include "MotionPolicy.h"
#include "TabBar.h"

namespace {

class MotionPolicyStateGuard {
public:
    MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
    }
};

QVector<QRect> tabRects(const TabBar &tabBar) {
    QVector<QRect> result;
    for (int index = 0; index < tabBar.count(); ++index)
        result.append(tabBar.tabRect(index));
    return result;
}

qreal progress(const QObject &object, const char *name) {
    return object.property(name).toReal();
}

struct ButtonState {
    QRect geometry;
    Qt::ArrowType direction = Qt::NoArrow;
    bool visible = false;
};

ButtonState buttonState(const QToolButton *button) {
    return {button->geometry(), button->arrowType(), button->isVisible()};
}

void expectEqual(const ButtonState &actual, const ButtonState &expected) {
    EXPECT_EQ(actual.geometry, expected.geometry);
    EXPECT_EQ(actual.direction, expected.direction);
    EXPECT_EQ(actual.visible, expected.visible);
}

QToolButton *nativeScrollButton(TabBar *tabBar, Qt::ArrowType direction) {
    for (QToolButton *button :
         tabBar->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button->arrowType() == direction)
            return button;
    }
    return nullptr;
}

struct OverflowControls {
    ButtonState nativeLeft;
    ButtonState nativeRight;
};

OverflowControls overflowControls(FilePanel *panel, TabBar *tabBar) {
    EXPECT_EQ(panel->findChild<QToolButton *>(QStringLiteral("PanelTabScrollLeftButton")),
              nullptr);
    QToolButton *nativeLeft = nativeScrollButton(tabBar, Qt::LeftArrow);
    QToolButton *nativeRight = nativeScrollButton(tabBar, Qt::RightArrow);
    EXPECT_NE(nativeLeft, nullptr);
    EXPECT_NE(nativeRight, nullptr);
    if (!nativeLeft || !nativeRight)
        return {};
    return {buttonState(nativeLeft), buttonState(nativeRight)};
}

TEST(TabMotion, ActivationProgressLeavesEveryTabRectUnchanged) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    TabBar tabBar;
    tabBar.addTab(QStringLiteral("One"));
    tabBar.addTab(QStringLiteral("Two"));
    tabBar.addTab(QStringLiteral("Three"));
    tabBar.resize(500, 36);
    tabBar.show();
    qApp->processEvents();

    ASSERT_GE(tabBar.metaObject()->indexOfProperty("activationProgress"), 0);
    const QVector<QRect> before = tabRects(tabBar);
    const QSize sizeHintBefore = tabBar.sizeHint();
    const QSize minimumSizeHintBefore = tabBar.minimumSizeHint();

    tabBar.setCurrentIndex(1);
    QTest::qWait(45);

    EXPECT_GT(progress(tabBar, "activationProgress"), 0.0);
    EXPECT_LT(progress(tabBar, "activationProgress"), 1.0);
    EXPECT_EQ(tabRects(tabBar), before);
    EXPECT_EQ(tabBar.sizeHint(), sizeHintBefore);
    EXPECT_EQ(tabBar.minimumSizeHint(), minimumSizeHintBefore);

    QTest::qWait(MotionPolicy::duration(MotionDuration::Fast));
    EXPECT_DOUBLE_EQ(progress(tabBar, "activationProgress"), 1.0);
    EXPECT_EQ(tabRects(tabBar), before);
    EXPECT_EQ(tabBar.sizeHint(), sizeHintBefore);
    EXPECT_EQ(tabBar.minimumSizeHint(), minimumSizeHintBefore);
}

TEST(TabMotion, NewTabActivatesAfterItsStateIsInstalled) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();

    TabBar *tabBar = panel.findChild<TabBar *>();
    ASSERT_NE(tabBar, nullptr);
    ASSERT_EQ(tabBar->count(), 1);

    panel.newTab();

    EXPECT_EQ(tabBar->count(), 2);
    EXPECT_EQ(tabBar->currentIndex(), 1);
    QTest::qWait(45);
    EXPECT_GT(progress(*tabBar, "activationProgress"), 0.0);
    EXPECT_LT(progress(*tabBar, "activationProgress"), 1.0);

    QTest::qWait(MotionPolicy::duration(MotionDuration::Fast));
    EXPECT_DOUBLE_EQ(progress(*tabBar, "activationProgress"), 1.0);
}

TEST(TabMotion, RestoredActiveTabActivatesAfterStateIsInstalled) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();

    TabBar *tabBar = panel.findChild<TabBar *>();
    ASSERT_NE(tabBar, nullptr);
    ASSERT_EQ(tabBar->count(), 1);

    panel.restoreTabs({{QDir::rootPath(), {}}, {QDir::homePath(), {}}}, 1);

    EXPECT_EQ(tabBar->count(), 2);
    EXPECT_EQ(tabBar->currentIndex(), 1);
    QTest::qWait(45);
    EXPECT_GT(progress(*tabBar, "activationProgress"), 0.0);
    EXPECT_LT(progress(*tabBar, "activationProgress"), 1.0);

    QTest::qWait(MotionPolicy::duration(MotionDuration::Fast));
    EXPECT_DOUBLE_EQ(progress(*tabBar, "activationProgress"), 1.0);
}

TEST(TabMotion, FocusProgressLeavesDualPaneSplitterSizesUnchanged) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    QSplitter splitter(Qt::Horizontal);
    auto *left = new FilePanel(&splitter);
    auto *right = new FilePanel(&splitter);
    splitter.addWidget(left);
    splitter.addWidget(right);
    splitter.resize(1000, 700);
    splitter.show();
    qApp->processEvents();

    ASSERT_GE(left->metaObject()->indexOfProperty("focusProgress"), 0);
    left->view()->setFocus();
    QTest::qWait(MotionPolicy::duration(MotionDuration::Fast));
    const QList<int> before = splitter.sizes();
    const QSize leftSizeHintBefore = left->sizeHint();
    const QSize leftMinimumSizeHintBefore = left->minimumSizeHint();
    const QSize rightSizeHintBefore = right->sizeHint();
    const QSize rightMinimumSizeHintBefore = right->minimumSizeHint();

    right->view()->setFocus();
    QTest::qWait(45);

    EXPECT_GT(progress(*right, "focusProgress"), 0.0);
    EXPECT_LT(progress(*right, "focusProgress"), 1.0);
    EXPECT_EQ(splitter.sizes(), before);
    EXPECT_EQ(left->sizeHint(), leftSizeHintBefore);
    EXPECT_EQ(left->minimumSizeHint(), leftMinimumSizeHintBefore);
    EXPECT_EQ(right->sizeHint(), rightSizeHintBefore);
    EXPECT_EQ(right->minimumSizeHint(), rightMinimumSizeHintBefore);

    QTest::qWait(MotionPolicy::duration(MotionDuration::Fast));
    EXPECT_DOUBLE_EQ(progress(*right, "focusProgress"), 1.0);
    EXPECT_DOUBLE_EQ(progress(*left, "focusProgress"), 0.0);
    EXPECT_EQ(splitter.sizes(), before);
    EXPECT_EQ(left->sizeHint(), leftSizeHintBefore);
    EXPECT_EQ(left->minimumSizeHint(), leftMinimumSizeHintBefore);
    EXPECT_EQ(right->sizeHint(), rightSizeHintBefore);
    EXPECT_EQ(right->minimumSizeHint(), rightMinimumSizeHintBefore);
}

TEST(TabMotion, OverflowControlsStayFixedDuringActivationProgress) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    QSplitter splitter(Qt::Horizontal);
    auto *left = new FilePanel(&splitter);
    auto *right = new FilePanel(&splitter);
    splitter.addWidget(left);
    splitter.addWidget(right);
    splitter.resize(600, 700);
    splitter.show();
    qApp->processEvents();

    for (FilePanel *panel : {left, right}) {
        for (int index = 0; index < 12; ++index)
            panel->newTab();
    }
    qApp->processEvents();
    qApp->processEvents();

    for (FilePanel *panel : {left, right}) {
        TabBar *tabBar = panel->findChild<TabBar *>();
        ASSERT_NE(tabBar, nullptr);
        ASSERT_GE(tabBar->metaObject()->indexOfProperty("activationProgress"), 0);
        const OverflowControls before = overflowControls(panel, tabBar);

        tabBar->setCurrentIndex(0);
        QTest::qWait(45);

        EXPECT_GT(progress(*tabBar, "activationProgress"), 0.0);
        EXPECT_LT(progress(*tabBar, "activationProgress"), 1.0);
        const OverflowControls during = overflowControls(panel, tabBar);
        expectEqual(during.nativeLeft, before.nativeLeft);
        expectEqual(during.nativeRight, before.nativeRight);
    }
}

TEST(TabMotion, ReducedMotionAppliesFinalTabAndFocusStatesSynchronously) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    QSplitter splitter(Qt::Horizontal);
    auto *left = new FilePanel(&splitter);
    auto *right = new FilePanel(&splitter);
    splitter.addWidget(left);
    splitter.addWidget(right);
    splitter.resize(1000, 700);
    splitter.show();
    qApp->processEvents();

    TabBar *tabs = left->findChild<TabBar *>();
    ASSERT_NE(tabs, nullptr);
    left->newTab();
    right->view()->setFocus();
    qApp->processEvents();

    EXPECT_DOUBLE_EQ(progress(*tabs, "activationProgress"), 1.0);
    EXPECT_DOUBLE_EQ(progress(*right, "focusProgress"), 1.0);
    EXPECT_DOUBLE_EQ(progress(*left, "focusProgress"), 0.0);
}

} // namespace
