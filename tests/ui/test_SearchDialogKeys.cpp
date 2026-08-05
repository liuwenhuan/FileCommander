#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QListWidget>
#include <QTest>

#include <memory>

#include "SearchDialog.h"
#include "SearchEngine.h"

// Typing a pattern and pressing Return did nothing -- the search only ran when
// the button was clicked. Return is what a search box is expected to answer to,
// and every field in this dialog is part of "what to search for", so it has to
// work from whichever one the user happens to be standing in.
namespace {

QPushButton *searchButton(SearchDialog &dialog) {
    for (QPushButton *button : dialog.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Search")
            || button->text() == QStringLiteral("Stop search"))
            return button;
    return nullptr;
}

// Watches for a search having been STARTED, which is a fact that stays true.
//
// The obvious probe -- the button's label, which reads "Stop search" for
// exactly as long as a search is running -- is not usable here, because it is
// true only WHILE the search runs. These tests search a directory of three
// files, which the walk can finish inside the same processEvents() that
// delivers the key press, and the label is then already back to "Search". That
// made every one of these tests fail about one run in five, on an application
// that had done exactly the right thing.
//
// The engine's started() signal is the same event without the expiry date.
std::unique_ptr<QSignalSpy> watchForSearchStart(SearchDialog &dialog) {
    SearchEngine *engine = dialog.findChild<SearchEngine *>();
    if (!engine)
        return nullptr;
    return std::make_unique<QSignalSpy>(engine, &SearchEngine::started);
}

QLineEdit *fieldWith(SearchDialog &dialog, const QString &text) {
    for (QLineEdit *edit : dialog.findChildren<QLineEdit *>())
        if (edit->text() == text)
            return edit;
    return nullptr;
}

void makeTree(const QString &root) {
    QDir().mkpath(root);
    for (const char *name : {"alpha.txt", "beta.txt", "gamma.log"}) {
        QFile file(QDir(root).filePath(QString::fromLatin1(name)));
        if (file.open(QIODevice::WriteOnly))
            file.write("x");
    }
}

} // namespace

// Starting is not the same as finishing. QLineEdit emits returnPressed and
// then IGNORES the key, so the dialog's default button sees it too -- and this
// dialog's default button is the one that stops a running search. Assert the
// search actually produces its results, not merely that it began.
TEST(SearchDialogKeysTest, ReturnRunsTheSearchToCompletionInsteadOfCancellingIt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeTree(dir.path());

    SearchDialog dialog(dir.path());
    dialog.show();
    qApp->processEvents();

    QLineEdit *pattern = fieldWith(dialog, QStringLiteral("*"));
    ASSERT_NE(pattern, nullptr);
    pattern->setFocus();
    pattern->setText(QStringLiteral("*.txt"));

    QTest::keyClick(pattern, Qt::Key_Return);

    auto *results = dialog.findChild<QListWidget *>();
    ASSERT_NE(results, nullptr);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 8000 && results->count() < 2)
        qApp->processEvents(QEventLoop::AllEvents, 20);

    EXPECT_GE(results->count(), 2)
        << "Return started the search and then something stopped it: "
        << results->count() << " result(s) after " << clock.elapsed() << "ms";
}

TEST(SearchDialogKeysTest, ReturnInTheNamePatternStartsTheSearch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeTree(dir.path());

    SearchDialog dialog(dir.path());
    dialog.show();
    qApp->processEvents();

    const auto watch = watchForSearchStart(dialog);
    ASSERT_NE(watch, nullptr) << "the dialog has no SearchEngine to watch";

    QLineEdit *pattern = fieldWith(dialog, QStringLiteral("*"));
    ASSERT_NE(pattern, nullptr);
    pattern->setFocus();
    pattern->setText(QStringLiteral("*.txt"));

    QTest::keyClick(pattern, Qt::Key_Return);
    qApp->processEvents();

    EXPECT_EQ(watch->count(), 1) << "Return in the pattern field did not search";
}

// The directory field is the other half of the same question. Standing in it
// and pressing Return is at least as natural as doing so in the pattern.
TEST(SearchDialogKeysTest, ReturnInTheDirectoryFieldStartsTheSearch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeTree(dir.path());

    SearchDialog dialog(dir.path());
    dialog.show();
    qApp->processEvents();

    const auto watch = watchForSearchStart(dialog);
    ASSERT_NE(watch, nullptr) << "the dialog has no SearchEngine to watch";

    QLineEdit *path = fieldWith(dialog, dir.path());
    ASSERT_NE(path, nullptr);
    path->setFocus();

    QTest::keyClick(path, Qt::Key_Return);
    qApp->processEvents();

    EXPECT_EQ(watch->count(), 1) << "Return in the directory field did not search";
}

// Whatever Return does, it must not be "close the dialog" -- QDialog's default
// answer to Return is accept(), and a search box that vanishes when you press
// Return is worse than one that ignores it.
TEST(SearchDialogKeysTest, ReturnNeverDismissesTheDialog) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeTree(dir.path());

    SearchDialog dialog(dir.path());
    dialog.show();
    qApp->processEvents();

    QLineEdit *pattern = fieldWith(dialog, QStringLiteral("*"));
    ASSERT_NE(pattern, nullptr);
    pattern->setFocus();
    QTest::keyClick(pattern, Qt::Key_Return);
    qApp->processEvents();

    EXPECT_TRUE(dialog.isVisible());
}

// A checkbox is still part of the search form; Return there should run the
// search rather than toggle nothing and be swallowed.
TEST(SearchDialogKeysTest, ReturnOnACheckboxStartsTheSearchToo) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeTree(dir.path());

    SearchDialog dialog(dir.path());
    dialog.show();
    qApp->processEvents();

    const auto watch = watchForSearchStart(dialog);
    ASSERT_NE(watch, nullptr) << "the dialog has no SearchEngine to watch";

    auto *check = dialog.findChild<QCheckBox *>();
    ASSERT_NE(check, nullptr);
    check->setFocus();

    QTest::keyClick(check, Qt::Key_Return);
    qApp->processEvents();

    EXPECT_EQ(watch->count(), 1) << "Return on a checkbox did not search";
}
