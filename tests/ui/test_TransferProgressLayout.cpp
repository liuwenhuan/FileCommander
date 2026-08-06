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
