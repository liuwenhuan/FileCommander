#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPalette>
#include <QSignalSpy>
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
    }

    ~MotionPolicyStateGuard() {
        MotionPolicy::clearReducedForTest();
        MotionPolicy::clearSystemReducedForTest();
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

TEST(NetworkStateMotion, ConnectingAppearsOnlyAfterNormalDelayBoundary) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();

    QLabel *indicator = connectionStatusLabel(panel);
    ASSERT_NE(indicator, nullptr);

    emitNetworkState(panel, NetworkSession::Connecting);
    QTest::qWait(120);
    EXPECT_FALSE(indicator->isVisible());

    FC_TRY_VERIFY_WITH_TIMEOUT(indicator->isVisible(), 2000);
    QVariantAnimation *colorAnimation =
        panel.findChild<QVariantAnimation *>(QStringLiteral("NetworkStatusColorAnimation"));
    ASSERT_NE(colorAnimation, nullptr);
    EXPECT_EQ(colorAnimation->duration(), 150);
    EXPECT_EQ(colorAnimation->state(), QAbstractAnimation::Running);
}

TEST(NetworkStateMotion, FailureAndRetryAreAvailableImmediately) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    FilePanel panel;
    panel.resize(700, 500);
    panel.show();
    qApp->processEvents();

    QLabel *indicator = connectionStatusLabel(panel);
    StatusBarWidget *statusBar = panel.findChild<StatusBarWidget *>();
    ASSERT_NE(indicator, nullptr);
    ASSERT_NE(statusBar, nullptr);
    QSignalSpy retrySpy(statusBar, &StatusBarWidget::retryRequested);

    emitNetworkState(panel, NetworkSession::Connecting);
    EXPECT_FALSE(indicator->isVisible());

    emitNetworkState(panel, NetworkSession::Failed);
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_TRUE(indicator->text().contains(QStringLiteral("#retry")));

    ASSERT_TRUE(QMetaObject::invokeMethod(indicator, "linkActivated", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("#retry"))));
    EXPECT_EQ(retrySpy.count(), 1);
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

    // Waits for the indicator, not for 175 ms.
    //
    // The reveal timer is 150 ms (FilePanel), so a fixed 175 ms wait left
    // 25 ms of margin -- which is no margin at all inside the full ui_tests
    // suite, where other tests' worker threads are still running and a timer
    // can be delivered late. It failed on CI with the indicator invisible and
    // its text empty, i.e. the timer simply had not been serviced yet.
    //
    // The assertion is unchanged in substance: an indicator that never appears
    // still fails, after a budget long enough to be about the application
    // rather than the load average.
    QElapsedTimer reveal;
    reveal.start();
    while (!indicator->isVisible() && reveal.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    EXPECT_TRUE(indicator->isVisible()) << "the connecting indicator never appeared";
    EXPECT_FALSE(indicator->text().isEmpty());
    QVariantAnimation *colorAnimation =
        panel.findChild<QVariantAnimation *>(QStringLiteral("NetworkStatusColorAnimation"));
    ASSERT_NE(colorAnimation, nullptr);
    EXPECT_NE(colorAnimation->state(), QAbstractAnimation::Running);
}

TEST(NetworkStateMotion, ConnectionStatusTextUsesThemeSemanticColors) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};

    QFile theme(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/green.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));

    StatusBarWidget statusBar;
    statusBar.setConnectionStatus(QStringLiteral("Connection failed"),
                                  StatusBarWidget::ConnFailed);
    qApp->processEvents();

    QLabel *indicator = nullptr;
    for (QLabel *label : statusBar.findChildren<QLabel *>()) {
        if (label->textFormat() == Qt::RichText) {
            indicator = label;
            break;
        }
    }
    ASSERT_NE(indicator, nullptr);
    EXPECT_EQ(indicator->property("semanticState").toString(), QStringLiteral("error"));
    EXPECT_EQ(indicator->palette().color(QPalette::WindowText), QColor(0x33, 0xff, 0x88));
    EXPECT_FALSE(indicator->text().contains(QStringLiteral("style='color:#")));
}

} // namespace
