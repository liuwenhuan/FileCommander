#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QTest>
#include <QVariantAnimation>

#include "FilePanel.h"
#include "MotionPolicy.h"
#include "StatusBarWidget.h"
#include "network/NetworkSession.h"

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

QLabel *connectionStatusLabel(FilePanel &panel) {
    StatusBarWidget *statusBar = panel.findChild<StatusBarWidget *>();
    if (!statusBar)
        return nullptr;

    for (QLabel *label : statusBar->findChildren<QLabel *>()) {
        if (label->textFormat() == Qt::RichText)
            return label;
    }
    return nullptr;
}

void emitNetworkState(FilePanel &panel, NetworkSession::State state) {
    panel.model()->networkStateChanged(static_cast<int>(state), 0);
    qApp->processEvents();
}

TEST(NetworkStateMotion, ConnectingThatCompletesBeforeDelayNeverShowsIndicator) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();

    QLabel *indicator = connectionStatusLabel(panel);
    ASSERT_NE(indicator, nullptr);
    ASSERT_FALSE(indicator->isVisible());

    emitNetworkState(panel, NetworkSession::Connecting);
    EXPECT_FALSE(indicator->isVisible());

    QTest::qWait(75);
    emitNetworkState(panel, NetworkSession::Connected);
    QTest::qWait(100);

    EXPECT_FALSE(indicator->isVisible());
}

TEST(NetworkStateMotion, ConnectingBeyondDelayShowsOneStaticIndicatorWithReducedMotion) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();

    QLabel *indicator = connectionStatusLabel(panel);
    ASSERT_NE(indicator, nullptr);

    emitNetworkState(panel, NetworkSession::Connecting);
    EXPECT_FALSE(indicator->isVisible());

    QTest::qWait(175);

    EXPECT_TRUE(indicator->isVisible());
    EXPECT_FALSE(indicator->text().isEmpty());
    QVariantAnimation *colorAnimation =
        panel.findChild<QVariantAnimation *>(QStringLiteral("NetworkStatusColorAnimation"));
    ASSERT_NE(colorAnimation, nullptr);
    EXPECT_NE(colorAnimation->state(), QAbstractAnimation::Running);
}

} // namespace
