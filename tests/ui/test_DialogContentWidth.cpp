#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMargins>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>

#include <memory>

#include "ThemedDialogs.h"
#include "dialogs/OperationProgressDialog.h"
#include "dialogs/TransferProgressDialog.h"

// A themed dialog's width has to follow how wide its text is *after wrapping*,
// never the longest unbroken string somebody interpolated into it. The reported
// defect: a copy failure naming two absolute paths raised a warning that spanned
// nearly the whole screen while the text kept wrapping in a narrow column,
// leaving a wide empty band beside every line.
//
// The cause is that Qt's own dialogs settle on their wrapped width only in their
// show event, whereas the frameless wrapper is laid out (and, via
// QLayout's default constraint, has its *minimum* width pinned) from the
// unwrapped hint beforehand. Measured on an 800x600 screen before the fix: the
// wrapper's minimum width was 5337 px for the message below, and resize(640)
// was clamped straight back up to it.
namespace {

// The frameless chrome draws a translucent shadow band outside the content, so
// the window is always this much wider than what it wraps. Read it off the
// dialog rather than hard-coding it, so the tests measure the empty band and not
// the frame.
int chromeWidth(const QWidget *dialog)
{
    const QMargins margins = dialog->contentsMargins();
    return margins.left() + margins.right();
}

QRect availableGeometryFor(const QWidget *dialog)
{
    const QScreen *dialogScreen = dialog->screen();
    if (!dialogScreen)
        dialogScreen = QGuiApplication::primaryScreen();
    return dialogScreen ? dialogScreen->availableGeometry() : QRect();
}

// The shape of the reported message: a sentence plus two long absolute paths,
// which is what MainWindow's "Operation Error" report joins together.
QString copyFailureMessage()
{
    return QStringLiteral(
        "Failed to copy "
        "/home/deepin/Documents/projects/filecommander/build/linux-debug/artifacts/"
        "reports/2026-07-31/very-long-source-directory-name/source-report-final.txt"
        " to "
        "/media/deepin/BackupVolume/archive/2026/july/incoming/staging/"
        "very-long-destination-directory-name/source-report-final.txt"
        ": Permission denied");
}

// One ordinary backup path -- nothing pathological, just long.
QString longWorkingPath()
{
    return QStringLiteral("/media/deepin/BackupVolume/archive/2026/july/incoming/staging/"
                          "a-directory-whose-name-really-does-go-on-and-on-like-this/"
                          "an-exported-report-file-with-a-very-long-name-2026-07-31-final.txt");
}

} // namespace

// The window is laid out once from the embedded box's pre-show size hint, which
// is the message on a single unwrapped line. It must not take that width, and --
// the half that actually bit -- it must not be left with it as a minimum, or
// nothing later can bring it back down.
TEST(DialogContentWidthTest, MessageWindowIsNotSizedByTheUnwrappedMessage)
{
    const std::unique_ptr<QDialog> dlg(ttc::createMessageDialog(
        nullptr, QMessageBox::Warning, QStringLiteral("Operation Error"), copyFailureMessage()));
    ASSERT_NE(dlg, nullptr);
    auto *box = dlg->findChild<QMessageBox *>();
    ASSERT_NE(box, nullptr);
    ASSERT_NE(dlg->layout(), nullptr);

    // The layout pass QWidget::setVisible() runs before any child is shown, i.e.
    // while the box still reports its unwrapped text as its size.
    dlg->layout()->activate();

    const QRect available = availableGeometryFor(dlg.get());
    ASSERT_FALSE(available.isEmpty());
    EXPECT_LE(dlg->width(), available.width() * 2 / 3)
        << "window " << dlg->width() << " on a " << available.width() << " px screen";
    EXPECT_LE(dlg->minimumWidth(), available.width() * 2 / 3)
        << "minimum width " << dlg->minimumWidth() << " pins the window to its message";
}

// Once the box has settled on its own wrapped width -- which is what
// QMessageBoxPrivate::updateSize() ends its show event by doing -- the window
// has to be exactly that plus the frame. Anything more is the empty band the
// user reported.
TEST(DialogContentWidthTest, MessageWindowFollowsTheSettledMessageBox)
{
    const std::unique_ptr<QDialog> dlg(ttc::createMessageDialog(
        nullptr, QMessageBox::Warning, QStringLiteral("Operation Error"), copyFailureMessage()));
    ASSERT_NE(dlg, nullptr);
    auto *box = dlg->findChild<QMessageBox *>();
    ASSERT_NE(box, nullptr);
    ASSERT_NE(dlg->layout(), nullptr);

    dlg->layout()->activate();

    // Stand in for the box's show event: QMessageBoxPrivate::updateSize() enables
    // word wrap, clamps the width against the screen, and calls setFixedSize.
    // Driving the real one is not an option here -- QMessageBox::showEvent
    // dereferences the platform native interface on Windows, and the offscreen
    // plugin has none.
    constexpr int kSettledBoxWidth = 500;
    box->setFixedSize(kSettledBoxWidth, 180);
    // The layout pass the dialog runs from its own show event, after the box has
    // settled and before the window is mapped.
    dlg->layout()->invalidate();
    dlg->layout()->activate();

    EXPECT_EQ(dlg->width(), kSettledBoxWidth + chromeWidth(dlg.get()))
        << "empty band of " << (dlg->width() - kSettledBoxWidth - chromeWidth(dlg.get()))
        << " px beside a " << kSettledBoxWidth << " px message box";
}

// The same property through message() itself, on a platform that can show a
// QMessageBox: the window is no wider than the box it wraps, and the box spends
// that width on text rather than on empty space.
//
// Skipped under the offscreen plugin on Windows: QMessageBox::showEvent calls
// GetSystemMenu through QGuiApplication::platformNativeInterface(), which that
// plugin does not provide, so showing any QMessageBox there takes the process
// down. Run with QT_QPA_PLATFORM=windows to exercise this; on Linux the same Qt
// code is not compiled and it runs under offscreen as it stands.
TEST(DialogContentWidthTest, ShownWarningHasNoEmptyBandBesideItsText)
{
#ifdef Q_OS_WIN
    if (!QGuiApplication::platformNativeInterface()) {
        GTEST_SKIP() << "the " << qPrintable(QGuiApplication::platformName())
                     << " plugin cannot show a QMessageBox on Windows";
    }
#endif

    int dialogWidth = 0;
    int boxWidth = 0;
    int labelWidth = 0;
    int chrome = 0;

    QTimer::singleShot(0, [&] {
        QWidget *modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        auto *box = modal->findChild<QMessageBox *>();
        ASSERT_NE(box, nullptr);
        auto *label = box->findChild<QLabel *>(QStringLiteral("qt_msgbox_label"));
        ASSERT_NE(label, nullptr);

        dialogWidth = modal->width();
        boxWidth = box->width();
        labelWidth = label->width();
        chrome = chromeWidth(modal);

        QAbstractButton *okButton = box->button(QMessageBox::Ok);
        ASSERT_NE(okButton, nullptr);
        okButton->click();
    });

    ttc::warning(nullptr, QStringLiteral("Operation Error"), copyFailureMessage());

    ASSERT_GT(dialogWidth, 0);
    ASSERT_GT(boxWidth, 0);
    EXPECT_EQ(dialogWidth, boxWidth + chrome)
        << "empty band of " << (dialogWidth - boxWidth - chrome) << " px: window " << dialogWidth
        << ", message box " << boxWidth << ", frame " << chrome;
    // The other half of "the width matches the content": a window sized to its
    // box is still wrong if the box then wraps at half its own width. Only the
    // icon column and the layout margins are entitled to the difference.
    EXPECT_GT(labelWidth, boxWidth / 2)
        << "text column " << labelWidth << " inside a " << boxWidth << " px message box";
}

// getText()/getInt() pinned a *minimum* width to an unbounded sizeHint. The
// intent was a comfortable floor for a short prompt; the effect on a long one
// was a window born too wide that the user could not then shrink. Unlike
// QMessageBox, QInputDialog shows fine under every plugin, so this one is
// measured on the real window.
TEST(DialogContentWidthTest, TextPromptWithALongLabelIsBoundedAndStillShrinkable)
{
    const QString label =
        QStringLiteral("Rename "
                       "/home/deepin/Documents/projects/filecommander/build/linux-debug/"
                       "artifacts/reports/2026-07-31/a-very-long-directory-name/"
                       "the-original-file-name-that-goes-on.txt"
                       " to:");

    int dialogWidth = 0;
    int minimumWidth = 0;
    QRect available;

    QTimer::singleShot(0, [&] {
        QWidget *modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        dialogWidth = modal->width();
        minimumWidth = modal->minimumWidth();
        available = availableGeometryFor(modal);
        static_cast<QDialog *>(modal)->reject();
    });

    bool ok = true;
    const QString entered = ttc::getText(nullptr, QStringLiteral("Rename"), label,
                                         QLineEdit::Normal, QStringLiteral("name.txt"), &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(entered.isEmpty());

    ASSERT_GT(dialogWidth, 0);
    ASSERT_FALSE(available.isEmpty());
    EXPECT_LE(dialogWidth, available.width() * 2 / 3)
        << "window " << dialogWidth << " on a " << available.width() << " px screen";
    // 360 is the comfortable floor for a short prompt that the code already
    // documented; the label's own length must not raise it.
    EXPECT_LE(minimumWidth, 360) << "minimum width " << minimumWidth << " above the 360 px floor";
}

// The same class of defect in the two progress windows, which is where a user
// meets it most often: every copy and move puts a destination directory in the
// description line and a file in the current-file line. Measured before the fix,
// on an 800 px screen: 853 px wide with a minimum of 853, from a window that
// asked for 460.
// Deliberately a comparison against the same window fed a short path, not an
// absolute figure: these dialogs also carry a bounded status line ("1 of 3
// items · 100 B / 1000 B · 1.2 MB/s · ETA 3s · elapsed 1s", 782 px measured)
// which legitimately wants room. What must not happen is the *path* adding to
// that, since a path has no length anybody controls.
TEST(DialogContentWidthTest, OperationProgressWidthDoesNotDependOnThePathLength)
{
    const auto measure = [](const QString &path) {
        OperationProgressDialog dialog;
        dialog.setDescription(QStringLiteral("Copying 3 item(s) to ") + path);
        dialog.setProgress(1, 3, 100, 1000, path);
        if (dialog.layout())
            dialog.layout()->activate();
        return QSize(dialog.width(), dialog.minimumWidth());
    };

    const QSize shortPath = measure(QStringLiteral("/tmp/a.txt"));
    const QSize longPath = measure(longWorkingPath());
    ASSERT_GT(shortPath.width(), 0);
    EXPECT_EQ(longPath.width(), shortPath.width())
        << "the path widened the window from " << shortPath.width() << " to " << longPath.width();
    EXPECT_EQ(longPath.height(), shortPath.height())
        << "the path raised the window's minimum width from " << shortPath.height() << " to "
        << longPath.height();
}

TEST(DialogContentWidthTest, TransferProgressWidthDoesNotDependOnThePathLength)
{
    const auto measure = [](const QString &path) {
        TransferProgressDialog dialog(nullptr);
        // Private slots, driven the way test_OperationProgressMotion already
        // drives this dialog: it wires itself to an OperationQueue rather than
        // exposing setters.
        QMetaObject::invokeMethod(&dialog, "onStarted", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("Copying 3 item(s) to ") + path));
        QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection, Q_ARG(qint64, 1),
                                  Q_ARG(qint64, 3), Q_ARG(qint64, 100), Q_ARG(qint64, 1000),
                                  Q_ARG(QString, path));
        QMetaObject::invokeMethod(&dialog, "onErrorOccurred", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("Permission denied: ") + path));
        if (dialog.layout())
            dialog.layout()->activate();
        return QSize(dialog.width(), dialog.minimumWidth());
    };

    const QSize shortPath = measure(QStringLiteral("/tmp/a.txt"));
    const QSize longPath = measure(longWorkingPath());
    ASSERT_GT(shortPath.width(), 0);
    EXPECT_EQ(longPath.width(), shortPath.width())
        << "the path widened the window from " << shortPath.width() << " to " << longPath.width();
    EXPECT_EQ(longPath.height(), shortPath.height())
        << "the path raised the window's minimum width from " << shortPath.height() << " to "
        << longPath.height();
}

// The short prompt the floor exists for still gets it, so the fix above did not
// simply trade one bad width for another.
TEST(DialogContentWidthTest, ShortTextPromptKeepsItsComfortableFloor)
{
    int dialogWidth = 0;
    QRect available;

    QTimer::singleShot(0, [&] {
        QWidget *modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        dialogWidth = modal->width();
        available = availableGeometryFor(modal);
        static_cast<QDialog *>(modal)->reject();
    });

    bool ok = true;
    ttc::getText(nullptr, QStringLiteral("Rename"), QStringLiteral("New name:"),
                 QLineEdit::Normal, QStringLiteral("a.txt"), &ok);

    ASSERT_FALSE(available.isEmpty());
    EXPECT_GE(dialogWidth, qMin(360, available.width()))
        << "window " << dialogWidth << " is narrower than the prompt floor";
}
