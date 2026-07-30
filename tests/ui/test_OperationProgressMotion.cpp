#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QPointer>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QTest>
#include <QTimer>
#include <QVariantAnimation>

#include "dialogs/OperationProgressDialog.h"
#include "dialogs/TransferProgressDialog.h"
#include "MotionPolicy.h"

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

void startTransfer(TransferProgressDialog &dialog, const QString &description) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onStarted", Qt::DirectConnection,
                                          Q_ARG(QString, description)));
}

void updateTransfer(TransferProgressDialog &dialog, qint64 doneItems, qint64 totalItems) {
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &dialog, "onProgress", Qt::DirectConnection, Q_ARG(qint64, doneItems),
        Q_ARG(qint64, totalItems), Q_ARG(qint64, 0), Q_ARG(qint64, 0),
        Q_ARG(QString, QStringLiteral("one"))));
    qApp->processEvents();
}

void finishTransfer(TransferProgressDialog &dialog, bool ok) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onFinished", Qt::DirectConnection,
                                          Q_ARG(bool, ok)));
    qApp->processEvents();
}

TEST(OperationProgressMotion, ProgressValuesAreAppliedImmediatelyDuringOperationReveal) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    OperationProgressDialog dialog;
    dialog.setDescription(QStringLiteral("Copying"));
    dialog.show();
    qApp->processEvents();
    QTest::qWait(20);

    QPropertyAnimation *reveal = dialog.findChild<QPropertyAnimation *>(
        QStringLiteral("OperationProgressRevealAnimation"));
    ASSERT_NE(reveal, nullptr);
    EXPECT_EQ(reveal->duration(), 120);
    EXPECT_EQ(reveal->state(), QAbstractAnimation::Running);

    QProgressBar *progress = dialog.findChild<QProgressBar *>();
    ASSERT_NE(progress, nullptr);

    dialog.setProgress(10, 100, 10 * 1024, 100 * 1024, QStringLiteral("one"));
    EXPECT_EQ(progress->value(), 10);

    dialog.setProgress(70, 100, 70 * 1024, 100 * 1024, QStringLiteral("one"));
    EXPECT_EQ(progress->value(), 70);

    dialog.setProgress(100, 100, 100 * 1024, 100 * 1024, QStringLiteral("one"));
    EXPECT_EQ(progress->value(), 100);
}

TEST(OperationProgressMotion, TransferProgressValuesAreAppliedImmediatelyDuringReveal) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    TransferProgressDialog dialog(nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onStarted", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("Copying"))));

    QProgressBar *progress = dialog.findChild<QProgressBar *>();
    ASSERT_NE(progress, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &dialog, "onProgress", Qt::DirectConnection, Q_ARG(qint64, 100), Q_ARG(qint64, 1000),
        Q_ARG(qint64, 0), Q_ARG(qint64, 0), Q_ARG(QString, QStringLiteral("one"))));

    QPropertyAnimation *reveal = dialog.findChild<QPropertyAnimation *>(
        QStringLiteral("TransferProgressRevealAnimation"));
    ASSERT_NE(reveal, nullptr);
    EXPECT_EQ(reveal->duration(), 120);
    EXPECT_EQ(reveal->state(), QAbstractAnimation::Running);
    EXPECT_EQ(progress->value(), 100);

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &dialog, "onProgress", Qt::DirectConnection, Q_ARG(qint64, 700), Q_ARG(qint64, 1000),
        Q_ARG(qint64, 0), Q_ARG(qint64, 0), Q_ARG(QString, QStringLiteral("one"))));
    EXPECT_EQ(progress->value(), 700);

    ASSERT_TRUE(QMetaObject::invokeMethod(
        &dialog, "onProgress", Qt::DirectConnection, Q_ARG(qint64, 1000), Q_ARG(qint64, 1000),
        Q_ARG(qint64, 0), Q_ARG(qint64, 0), Q_ARG(QString, QStringLiteral("one"))));
    EXPECT_EQ(progress->value(), 1000);
}

TEST(OperationProgressMotion, FastTransferNeverFlashesAfterFinishing) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    TransferProgressDialog dialog(nullptr);
    startTransfer(dialog, QStringLiteral("Quick copy"));
    finishTransfer(dialog, true);

    EXPECT_FALSE(dialog.isVisible());
    QTest::qWait(1050);
    EXPECT_FALSE(dialog.isVisible());
}

TEST(OperationProgressMotion, SuccessfulOutcomeRemainsVisibleFor180Milliseconds) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    TransferProgressDialog dialog(nullptr);
    startTransfer(dialog, QStringLiteral("Large copy"));
    updateTransfer(dialog, 100, 1000);
    ASSERT_TRUE(dialog.isVisible());

    finishTransfer(dialog, true);
    QVariantAnimation *outcome = dialog.findChild<QVariantAnimation *>(
        QStringLiteral("TransferProgressOutcomeColorAnimation"));
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->duration(), 180);
    EXPECT_EQ(outcome->state(), QAbstractAnimation::Running);

    QTest::qWait(130);
    EXPECT_TRUE(dialog.isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 100);
}

TEST(OperationProgressMotion, ReducedMotionKeepsStaticSuccessVisibleFor180Milliseconds) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    TransferProgressDialog dialog(nullptr);
    startTransfer(dialog, QStringLiteral("Large copy"));
    updateTransfer(dialog, 100, 1000);
    ASSERT_TRUE(dialog.isVisible());

    finishTransfer(dialog, true);
    QVariantAnimation *outcome = dialog.findChild<QVariantAnimation *>(
        QStringLiteral("TransferProgressOutcomeColorAnimation"));
    ASSERT_NE(outcome, nullptr);
    EXPECT_NE(outcome->state(), QAbstractAnimation::Running);
    EXPECT_EQ(dialog.findChild<QProgressBar *>()->palette().color(QPalette::Highlight),
              QColor(0x2c, 0xa0, 0x44));

    QTest::qWait(130);
    EXPECT_TRUE(dialog.isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 100);
}

TEST(OperationProgressMotion, OneConcurrentTransferFinishingDoesNotEndTheBatch) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    TransferProgressDialog dialog(nullptr);
    startTransfer(dialog, QStringLiteral("First copy"));
    startTransfer(dialog, QStringLiteral("Second copy"));
    updateTransfer(dialog, 100, 1000);
    ASSERT_TRUE(dialog.isVisible());

    finishTransfer(dialog, true);
    QTest::qWait(220);
    EXPECT_TRUE(dialog.isVisible());

    finishTransfer(dialog, true);
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 250);
}

TEST(OperationProgressMotion, TerminalTimerIsCancelledWhenDialogIsDestroyed) {
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(false);

    QPointer<TransferProgressDialog> dialog = new TransferProgressDialog(nullptr);
    startTransfer(*dialog, QStringLiteral("Large copy"));
    updateTransfer(*dialog, 100, 1000);
    finishTransfer(*dialog, true);

    QTimer *terminalTimer = dialog->findChild<QTimer *>(
        QStringLiteral("TransferProgressTerminalHideTimer"));
    ASSERT_NE(terminalTimer, nullptr);
    EXPECT_TRUE(terminalTimer->isActive());
    EXPECT_EQ(terminalTimer->interval(), 180);

    delete dialog;
    EXPECT_TRUE(dialog.isNull());
    QTest::qWait(200);
    SUCCEED();
}

} // namespace
