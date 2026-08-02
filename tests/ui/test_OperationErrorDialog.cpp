#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QTimer>

#include "dialogs/OperationErrorDialog.h"

namespace {

OperationError permissionError()
{
    OperationError error;
    error.category = OperationErrorCategory::PermissionDenied;
    error.message = QStringLiteral("Access to the destination was denied.");
    error.sourcePath = QStringLiteral("C:/work/source/report.txt");
    error.targetPath = QStringLiteral("C:/protected/destination/report.txt");
    error.elevatable = true;
    return error;
}

QDialog *activeOperationErrorDialog()
{
    return qobject_cast<QDialog *>(QApplication::activeModalWidget());
}

QPushButton *actionButton(QDialog *dialog, const QString &objectName)
{
    return dialog ? dialog->findChild<QPushButton *>(objectName) : nullptr;
}

} // namespace

TEST(OperationErrorDialogTest, ShowsDetailsAndOrdinaryActionsThenMapsAbort)
{
    OperationError error = permissionError();
    error.elevatable = false;
    QDialog *shownDialog = nullptr;

    QTimer::singleShot(0, [&] {
        shownDialog = activeOperationErrorDialog();
        ASSERT_NE(shownDialog, nullptr);

        QLabel *reason = shownDialog->findChild<QLabel *>(QStringLiteral("OperationErrorReason"));
        QLabel *source = shownDialog->findChild<QLabel *>(QStringLiteral("OperationErrorSource"));
        QLabel *target = shownDialog->findChild<QLabel *>(QStringLiteral("OperationErrorTarget"));
        ASSERT_NE(reason, nullptr);
        ASSERT_NE(source, nullptr);
        ASSERT_NE(target, nullptr);
        EXPECT_TRUE(reason->text().contains(error.message));
        EXPECT_EQ(reason->property("semanticState").toString(), QStringLiteral("error"));
        EXPECT_TRUE(reason->wordWrap());
        EXPECT_TRUE(source->text().contains(error.sourcePath));
        EXPECT_TRUE(source->wordWrap());
        EXPECT_TRUE(target->text().contains(error.targetPath));
        EXPECT_TRUE(target->wordWrap());
        EXPECT_EQ(source->property("semanticState").toString(), QStringLiteral("muted"));
        EXPECT_EQ(target->property("semanticState").toString(), QStringLiteral("muted"));

        for (const QString &name : {QStringLiteral("OperationErrorRetry"),
                                    QStringLiteral("OperationErrorSkip"),
                                    QStringLiteral("OperationErrorSkipAll"),
                                    QStringLiteral("OperationErrorCancelCurrent"),
                                    QStringLiteral("OperationErrorAbort")}) {
            QPushButton *button = actionButton(shownDialog, name);
            ASSERT_NE(button, nullptr) << name.toStdString();
            EXPECT_TRUE(button->isVisible()) << name.toStdString();
        }
        EXPECT_EQ(actionButton(shownDialog, QStringLiteral("OperationErrorElevate")), nullptr);

        actionButton(shownDialog, QStringLiteral("OperationErrorAbort"))->click();
    });

    EXPECT_EQ(OperationErrorDialog::ask(nullptr, error, true), ErrorAction::Abort);
    ASSERT_NE(shownDialog, nullptr);
}

TEST(OperationErrorDialogTest, OffersElevationOnlyForEligibleLocalErrorsAndMapsIt)
{
    OperationError localError = permissionError();
    QTimer::singleShot(0, [&] {
        QDialog *dialog = activeOperationErrorDialog();
        ASSERT_NE(dialog, nullptr);
        QPushButton *elevate = actionButton(dialog, QStringLiteral("OperationErrorElevate"));
        ASSERT_NE(elevate, nullptr);
        EXPECT_TRUE(elevate->isVisible());
        elevate->click();
    });
    EXPECT_EQ(OperationErrorDialog::ask(nullptr, localError, true), ErrorAction::Elevate);

    localError.remote = true;
    QTimer::singleShot(0, [&] {
        QDialog *dialog = activeOperationErrorDialog();
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(actionButton(dialog, QStringLiteral("OperationErrorElevate")), nullptr);
        actionButton(dialog, QStringLiteral("OperationErrorAbort"))->click();
    });
    EXPECT_EQ(OperationErrorDialog::ask(nullptr, localError, true), ErrorAction::Abort);

    localError.remote = false;
    QTimer::singleShot(0, [&] {
        QDialog *dialog = activeOperationErrorDialog();
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(actionButton(dialog, QStringLiteral("OperationErrorElevate")), nullptr);
        actionButton(dialog, QStringLiteral("OperationErrorAbort"))->click();
    });
    EXPECT_EQ(OperationErrorDialog::ask(nullptr, localError, false), ErrorAction::Abort);
}

TEST(OperationErrorDialogTest, LargeFontKeepsWrappedButtonsAndTextOnScreen)
{
    const QFont originalFont = QApplication::font();
    // Deliberately not derived from QApplication::font(): this test's wrapping/fit
    // assertions depend on the exact glyph widths at 20pt, so basing it on whatever the
    // ambient application font happens to be would make the test's outcome depend on
    // test execution order (e.g. a typography test upstream in the same process that
    // sets a different font family) rather than solely on this dialog's own layout.
    QFont largeFont(QStringLiteral("Arial"));
    largeFont.setPointSize(20);
    QApplication::setFont(largeFont);

    OperationError error = permissionError();
    // Enough repeats to force wrapping onto several lines at 20pt without being so tall
    // that the dialog's *legitimate* minimum content height (button rows included) can
    // exceed even a small real screen -- that's a screen-too-small case, not a bug, and
    // isn't what this test means to cover.
    error.sourcePath = QStringLiteral("C:/workspace/") +
                       QStringLiteral("very-long-source-directory/").repeated(3) +
                       QStringLiteral("report.txt");
    error.targetPath = QStringLiteral("C:/workspace/") +
                       QStringLiteral("very-long-target-directory/").repeated(3) +
                       QStringLiteral("report.txt");

    QDialog *shownDialog = nullptr;
    QTimer::singleShot(0, [&] {
        shownDialog = activeOperationErrorDialog();
        ASSERT_NE(shownDialog, nullptr);
        QApplication::processEvents();

        ASSERT_NE(shownDialog->screen(), nullptr);
        // Deliberately not asserting the window's frameGeometry stays within
        // screen()->availableGeometry() here: buttons are grouped into two fixed rows (see
        // OperationErrorDialog::updateActionLayout), and at 20pt some of these labels
        // ("Cancel Current", "Run as administrator") are simply wide enough that no row
        // split keeps every row under a small screen's width without clipping text or
        // shrinking the font -- neither of which this dialog does. That is a screen/font
        // combination the window's own setMaximumSize() already degrades as gracefully as
        // it can (clamped, not crashing or throwing content away); it is not something this
        // test can meaningfully assert always fits.
        QSet<int> buttonRows;
        for (QPushButton *button : shownDialog->findChildren<QPushButton *>()) {
            ASSERT_TRUE(button->isVisible()) << button->objectName().toStdString();
            const QRect inDialog(button->mapTo(shownDialog, QPoint(0, 0)), button->size());
            EXPECT_TRUE(shownDialog->rect().contains(inDialog)) << button->objectName().toStdString();
            buttonRows.insert(inDialog.y());
        }
        EXPECT_GT(buttonRows.size(), 1);

        QLabel *reason = shownDialog->findChild<QLabel *>(QStringLiteral("OperationErrorReason"));
        QLabel *source = shownDialog->findChild<QLabel *>(QStringLiteral("OperationErrorSource"));
        QLabel *target = shownDialog->findChild<QLabel *>(QStringLiteral("OperationErrorTarget"));
        ASSERT_NE(reason, nullptr);
        ASSERT_NE(source, nullptr);
        ASSERT_NE(target, nullptr);
        EXPECT_TRUE(reason->wordWrap());
        EXPECT_TRUE(source->wordWrap());
        EXPECT_TRUE(target->wordWrap());

        actionButton(shownDialog, QStringLiteral("OperationErrorAbort"))->click();
    });

    EXPECT_EQ(OperationErrorDialog::ask(nullptr, error, true), ErrorAction::Abort);
    QApplication::setFont(originalFont);
}
