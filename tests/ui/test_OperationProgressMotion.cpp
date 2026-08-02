#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariantAnimation>

#include "FilePanel.h"
#include "FileSystemModel.h"
#include "MainWindow.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/TransferProgressDialog.h"
#include "MotionPolicy.h"
#include "operations/OperationQueue.h"

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

FilePanel *panelAtPath(MainWindow &window, const QString &path) {
    const QString cleanPath = QDir::cleanPath(path);
    for (FilePanel *panel : window.findChildren<FilePanel *>()) {
        if (QDir::cleanPath(panel->currentPath()) == cleanPath)
            return panel;
    }
    return nullptr;
}

int rowNamed(FilePanel *panel, const QString &name) {
    for (int row = 0; row < panel->model()->rowCount(); ++row) {
        if (panel->model()->fileInfoAt(row).name() == name)
            return row;
    }
    return -1;
}

void writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(contents), contents.size());
}

TEST(OperationProgressMotion, MainWindowKeepsOperationQueueEagerButDefersTransferDialog) {
    MainWindow window;

    EXPECT_EQ(window.findChildren<OperationQueue *>().size(), 1);
    EXPECT_TRUE(window.findChildren<TransferProgressDialog *>().isEmpty());
}

TEST(OperationProgressMotion, FirstCopyCommandCreatesExactlyOneTransferDialog) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    QDir dir(root.path());
    ASSERT_TRUE(dir.mkdir(QStringLiteral("source")));
    ASSERT_TRUE(dir.mkdir(QStringLiteral("destination")));
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString destination = dir.filePath(QStringLiteral("destination"));
    writeFile(QDir(source).filePath(QStringLiteral("first.txt")), QByteArray("first"));
    writeFile(QDir(source).filePath(QStringLiteral("second.txt")), QByteArray("second"));

    MainWindow window;
    window.openFolders({source, destination});
    FilePanel *sourcePanel = panelAtPath(window, source);
    ASSERT_NE(sourcePanel, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(sourcePanel->model()->rowCount(), 2, 4000);
    ASSERT_EQ(window.findChildren<OperationQueue *>().size(), 1);
    EXPECT_TRUE(window.findChildren<TransferProgressDialog *>().isEmpty());

    int row = rowNamed(sourcePanel, QStringLiteral("first.txt"));
    ASSERT_GE(row, 0);
    sourcePanel->view()->setCurrentIndex(
        sourcePanel->model()->index(row, FileSystemModel::NameColumn));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "copySelected", Qt::DirectConnection));
    EXPECT_EQ(window.findChildren<TransferProgressDialog *>().size(), 1);

    row = rowNamed(sourcePanel, QStringLiteral("second.txt"));
    ASSERT_GE(row, 0);
    sourcePanel->view()->setCurrentIndex(
        sourcePanel->model()->index(row, FileSystemModel::NameColumn));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "copySelected", Qt::DirectConnection));
    EXPECT_EQ(window.findChildren<TransferProgressDialog *>().size(), 1);
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

TEST(OperationProgressMotion, AbortDismissesTransferWindowImmediately) {
    TransferProgressDialog dialog(nullptr);
    startTransfer(dialog, QStringLiteral("Copying"));
    dialog.show();
    qApp->processEvents();
    ASSERT_TRUE(dialog.isVisible());

    dialog.dismissAfterAbort();
    qApp->processEvents();

    EXPECT_FALSE(dialog.isVisible());
}

TEST(OperationProgressMotion, TransferErrorExpandsForWrappedTextAfterFontIncrease) {
    TransferProgressDialog dialog(nullptr);
    dialog.resize(460, 180);

    QFont largeFont = dialog.font();
    largeFont.setPointSize(20);
    dialog.setFont(largeFont);

    startTransfer(dialog, QStringLiteral("Moving one item"));
    updateTransfer(dialog, 0, 1000);
    ASSERT_TRUE(dialog.isVisible());

    const QString error = QStringLiteral(
        "Failed to copy C:/Program Files to C:/Program Files/Program Files");
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onErrorOccurred", Qt::DirectConnection,
                                          Q_ARG(QString, error)));
    qApp->processEvents();

    QLabel *errorLabel = nullptr;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text() == error) {
            errorLabel = label;
            break;
        }
    }
    ASSERT_NE(errorLabel, nullptr);
    ASSERT_GT(errorLabel->contentsRect().width(), 0);
    EXPECT_GE(errorLabel->contentsRect().height(),
              errorLabel->heightForWidth(errorLabel->contentsRect().width()));
}

TEST(OperationProgressMotion, TransferErrorUsesTheCurrentThemeColor) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};

    QFile theme(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/green.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));

    TransferProgressDialog dialog(nullptr);
    const QString error = QStringLiteral("Failed to move an item");
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onErrorOccurred", Qt::DirectConnection,
                                          Q_ARG(QString, error)));
    qApp->processEvents();

    QLabel *errorLabel = nullptr;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text() == error) {
            errorLabel = label;
            break;
        }
    }
    ASSERT_NE(errorLabel, nullptr);
    EXPECT_EQ(errorLabel->palette().color(QPalette::WindowText), QColor(0x33, 0xff, 0x88));
}

} // namespace
