#include <gtest/gtest.h>

#include <QApplication>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QTest>

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

} // namespace
