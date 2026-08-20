#include <gtest/gtest.h>

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTest>

#include "OperationProgressDialog.h"
#include "TransferProgressDialog.h"
#include "TranslationManager.h"
#include "operations/OperationQueue.h"

// The transfer dialog shows a source path that routinely runs past one line.
// It wraps, but the dialog opened at a fixed height and simply cut the second
// line off -- the text was there, the room for it was not. Only the error label
// had a fitting pass; the path did not.
namespace {

QLabel *labelShowing(const QWidget &dialog, const QString &needle) {
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text().contains(needle))
            return label;
    }
    return nullptr;
}

} // namespace

namespace {

// Drives the dialog the way OperationQueue does for a batch big enough to be
// revealed at once, and leaves it finished with the given outcome.
void runOneBatch(TransferProgressDialog &dialog, bool ok) {
    QMetaObject::invokeMethod(&dialog, "onStarted", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("Copying to /tmp")));
    QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                              Q_ARG(qint64, 0), Q_ARG(qint64, 1), Q_ARG(qint64, 0),
                              Q_ARG(qint64, 512LL * 1024 * 1024),
                              Q_ARG(QString, QStringLiteral("big.bin")));
    QTest::qWaitForWindowExposed(&dialog);
    ASSERT_TRUE(dialog.isVisible());
    QMetaObject::invokeMethod(&dialog, "onFinished", Qt::DirectConnection, Q_ARG(bool, ok));
}

} // namespace

// Skip and Cancel in the overwrite prompt end the batch as "not ok" without
// emitting any error, so this window used to sit there for good: a red bar, an
// empty message, and nothing that would ever take it down again.
TEST(TransferProgressLayout, AFailedBatchWithNoMessageStillClosesItself) {
    OperationQueue queue;
    TransferProgressDialog dialog(&queue);

    runOneBatch(dialog, /*ok=*/false);

    EXPECT_TRUE(QTest::qWaitFor([&dialog] { return !dialog.isVisible(); }, 2000));
}

// The other half of the same gate: a message the user has to read keeps the
// window up until they close it.
TEST(TransferProgressLayout, AReportedErrorKeepsTheWindowUp) {
    OperationQueue queue;
    TransferProgressDialog dialog(&queue);

    QMetaObject::invokeMethod(&dialog, "onStarted", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("Copying to /tmp")));
    QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                              Q_ARG(qint64, 0), Q_ARG(qint64, 1), Q_ARG(qint64, 0),
                              Q_ARG(qint64, 512LL * 1024 * 1024),
                              Q_ARG(QString, QStringLiteral("big.bin")));
    QTest::qWaitForWindowExposed(&dialog);
    QMetaObject::invokeMethod(&dialog, "onErrorOccurred", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("Permission denied")));
    QMetaObject::invokeMethod(&dialog, "onFinished", Qt::DirectConnection, Q_ARG(bool, false));

    QTest::qWait(400); // well past kOutcomeDurationMs
    EXPECT_TRUE(dialog.isVisible());
}

// A batch whose only work was one conflicting file must never put a window on
// screen at all: the deferred-show timer fires while the modal overwrite prompt
// holds the user, and releasing the suppression used to reveal the window a
// fraction of a second before the finished batch took it away again.
TEST(TransferProgressLayout, SkippingTheOnlyFileNeverFlashesTheWindow) {
    OperationQueue queue;
    TransferProgressDialog dialog(&queue);

    QMetaObject::invokeMethod(&dialog, "onStarted", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("Copying to /tmp")));
    dialog.suppressAutoShow(true); // the overwrite prompt is up
    QTest::qWait(1200);            // past kShowDelayMs: the show timer fires, suppressed
    ASSERT_FALSE(dialog.isVisible());

    dialog.suppressAutoShow(false); // user answered Skip
    QMetaObject::invokeMethod(&dialog, "onFinished", Qt::DirectConnection, Q_ARG(bool, false));

    QTest::qWait(300);
    EXPECT_FALSE(dialog.isVisible());
}

TEST(TransferProgressLayout, ALongPathIsGivenTheRoomToWrapInto) {
    OperationQueue queue;
    TransferProgressDialog dialog(&queue);
    dialog.show();
    QTest::qWaitForWindowExposed(&dialog);

    const QString deepPath =
        QStringLiteral("C:/Users/alice/Downloads/a-fairly-deep-folder/and-another-one/"
                       "and-a-third-for-good-measure/the-file-itself.bin");
    // Drive it the way the queue does: onProgress carries the current file.
    QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                              Q_ARG(qint64, 0), Q_ARG(qint64, 1), Q_ARG(qint64, 1024),
                              Q_ARG(qint64, 4096), Q_ARG(QString, deepPath));
    qApp->processEvents();

    QLabel *path = labelShowing(dialog, QStringLiteral("the-file-itself.bin"));
    ASSERT_NE(path, nullptr) << "the dialog is not showing the path at all";
    ASSERT_TRUE(path->wordWrap());

    // Two things, and the second is the one that was broken: the label must be
    // tall enough for the wrapped text, AND the dialog tall enough for the
    // label. A label sized correctly inside a dialog that clips it looks
    // exactly like the bug.
    const int needed = path->heightForWidth(path->width());
    EXPECT_GE(path->height(), needed)
        << "label is " << path->height() << "px for " << needed << "px of text";
    EXPECT_LE(path->geometry().bottom(), dialog.height())
        << "the path runs past the bottom of the dialog";
}

// The same defect lived in the sibling dialog, which shows the same kind of
// path against the same kind of fixed starting height.
TEST(TransferProgressLayout, TheOperationDialogGivesItsPathRoomToo) {
    OperationProgressDialog dialog;
    dialog.show();
    QTest::qWaitForWindowExposed(&dialog);

    const QString deepPath =
        QStringLiteral("C:/Users/alice/Downloads/a-fairly-deep-folder/and-another-one/"
                       "and-a-third-for-good-measure/the-file-itself.bin");
    dialog.setProgress(0, 1, 1024, 4096, deepPath);
    qApp->processEvents();

    QLabel *path = labelShowing(dialog, QStringLiteral("the-file-itself.bin"));
    ASSERT_NE(path, nullptr);
    EXPECT_GE(path->height(), path->heightForWidth(path->width()))
        << "label is " << path->height() << "px for "
        << path->heightForWidth(path->width()) << "px of text";
    EXPECT_LE(path->geometry().bottom(), dialog.height());
}
