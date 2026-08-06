#include <gtest/gtest.h>

#include "TryUntil.h"

#include <atomic>

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
#include <QPushButton>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariantAnimation>
#include <QtTest/QSignalSpy>

#include "FilePanel.h"
#include "FileSystemModel.h"
#include "MainWindow.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/TransferProgressDialog.h"
#include "MotionPolicy.h"
#include "operations/FileOperations.h"
#include "operations/OperationQueue.h"
#include "ThemeStateGuard.h"

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
    ThemeStateGuard themeState;
    MainWindow window;

    EXPECT_EQ(window.findChildren<OperationQueue *>().size(), 1);
    EXPECT_TRUE(window.findChildren<TransferProgressDialog *>().isEmpty());
}

TEST(OperationProgressMotion, FirstCopyCommandCreatesExactlyOneTransferDialog) {
    ThemeStateGuard themeState;
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
    // Waits for the two files by name rather than for a row count of 2. A
    // directory listing also carries the ".." parent row, so this is three
    // rows and never was two -- the wait ran its full four seconds every time
    // and then passed anyway, because the QTRY macro it was written with
    // cannot fail a gtest body.
    FC_TRY_VERIFY_WITH_TIMEOUT(rowNamed(sourcePanel, QStringLiteral("first.txt")) >= 0 &&
                                   rowNamed(sourcePanel, QStringLiteral("second.txt")) >= 0,
                               4000);
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    FC_TRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 2000);
}

TEST(OperationProgressMotion, ReducedMotionKeepsStaticSuccessVisibleFor180Milliseconds) {
    ThemeStateGuard themeState;
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
    FC_TRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 2000);
}

TEST(OperationProgressMotion, OneConcurrentTransferFinishingDoesNotEndTheBatch) {
    ThemeStateGuard themeState;
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
    FC_TRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 2000);
}

TEST(OperationProgressMotion, TerminalTimerIsCancelledWhenDialogIsDestroyed) {
    ThemeStateGuard themeState;
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

// End-to-end for the 中止 button: a real OperationQueue copying a real file,
// stopped from the real widget. The latch guarantees the click lands while the
// copy is genuinely in flight, which is what makes the assertions mean anything.
TEST(OperationProgressMotion, AbortButtonStopsTheRunningCopyAndClosesTheWindow) {
    ThemeStateGuard themeState;
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = QDir(srcDir.path()).filePath(QStringLiteral("abort-me.bin"));
    writeFile(source, QByteArray(24 * 1024 * 1024, 'x'));
    const QString destination = QDir(dstDir.path()).filePath(QStringLiteral("abort-me.bin"));

    OperationQueue queue;
    TransferProgressDialog dialog(&queue);

    // Start the (lazily created) local worker and leave it idle before installing
    // the hook it will read from its own thread.
    QSignalSpy warmed(&queue, &OperationQueue::finished);
    queue.enqueueMkdir(dstDir.path(), QStringLiteral("warm-up"));
    ASSERT_TRUE(warmed.count() > 0 || warmed.wait(3000));
    FileOperations *ops = queue.localOperationsForTesting();
    ASSERT_NE(ops, nullptr);

    std::atomic<bool> parked{false};
    QSemaphore entered;
    QSemaphore release;
    ops->setCopyChunkHookForTesting([&](qint64, qint64) {
        if (!parked.exchange(true)) {
            entered.release();
            release.acquire();
        }
    });

    QSignalSpy finished(&queue, &OperationQueue::finished);
    queue.enqueueCopy({source}, dstDir.path());
    ASSERT_TRUE(entered.tryAcquire(1, 10000));

    // The window reveals itself on its own deferred-show timer -- no faked
    // signals here, the whole wiring is under test.
    FC_TRY_VERIFY_WITH_TIMEOUT(dialog.isVisible(), 4000);

    auto *abortButton = dialog.findChild<QPushButton *>(QStringLiteral("TransferAbortButton"));
    ASSERT_NE(abortButton, nullptr);
    abortButton->click();
    qApp->processEvents();

    // Closed straight away, without waiting for the worker to unwind.
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(queue.queuedCount(), 0);

    release.release();
    FC_TRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
    EXPECT_FALSE(finished.takeFirst().at(0).toBool());
    EXPECT_FALSE(QFile::exists(destination));
    EXPECT_FALSE(dialog.isVisible());
}

// The window used to be a one-shot: hiding it any way other than
// dismissAfterAbort() left it flagged as "on screen" forever, so no later batch
// could ever reveal it again.
TEST(OperationProgressMotion, TransferWindowCanBeRevealedAgainAfterBeingClosed) {
    ThemeStateGuard themeState;
    MotionPolicyStateGuard guard;
    MotionPolicy::setReducedForTest(true);

    TransferProgressDialog dialog(nullptr);
    startTransfer(dialog, QStringLiteral("First batch"));
    updateTransfer(dialog, 100, 1000);
    ASSERT_TRUE(dialog.isVisible());

    dialog.close(); // what the title bar's own close button does
    qApp->processEvents();
    ASSERT_FALSE(dialog.isVisible());

    finishTransfer(dialog, true);
    startTransfer(dialog, QStringLiteral("Second batch"));
    updateTransfer(dialog, 100, 1000);
    EXPECT_TRUE(dialog.isVisible());
}

TEST(OperationProgressMotion, AbortDismissesTransferWindowImmediately) {
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
