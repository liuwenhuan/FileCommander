#include <gtest/gtest.h>

#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <QToolButton>

#include "ByteSearch.h"
#include "FindBar.h"

namespace {

struct Request {
    int count = 0;
    ByteSearch::Needle needle;
    ByteSearch::Direction direction = ByteSearch::Direction::Forward;
};

// Plain lambdas rather than QSignalSpy: the signal carries a struct and a
// scoped enum, and a spy would need both registered as metatypes just to
// observe a direct connection.
class Harness {
public:
    Harness() {
        QObject::connect(&bar, &FindBar::searchRequested,
                         [this](const ByteSearch::Needle &needle, ByteSearch::Direction dir) {
                             request.count++;
                             request.needle = needle;
                             request.direction = dir;
                         });
        QObject::connect(&bar, &FindBar::queryChanged,
                         [this](const ByteSearch::Needle &) { queryChanges++; });
        QObject::connect(&bar, &FindBar::closed, [this] { closedCount++; });
        bar.show();
    }

    QLineEdit *input() const { return bar.findChild<QLineEdit *>(QStringLiteral("FindBarInput")); }
    QToolButton *button(const char *name) const {
        return bar.findChild<QToolButton *>(QString::fromLatin1(name));
    }
    QLabel *label(const char *name) const {
        return bar.findChild<QLabel *>(QString::fromLatin1(name));
    }

    FindBar bar;
    Request request;
    int queryChanges = 0;
    int closedCount = 0;
};

} // namespace

TEST(FindBarTest, ExposesTheDocumentedChildObjectNames) {
    // The stylesheet selectors named in FindBar.h are the contract with the
    // three themes; renaming a child silently unstyles it in all of them.
    Harness h;
    EXPECT_NE(h.input(), nullptr);
    for (const char *name : {"FindBarHexToggle", "FindBarCaseToggle", "FindBarPrevButton",
                             "FindBarNextButton", "FindBarCloseButton"})
        EXPECT_NE(h.button(name), nullptr) << name;
    for (const char *name : {"FindBarStatus", "FindBarNote"})
        EXPECT_NE(h.label(name), nullptr) << name;
    EXPECT_EQ(h.bar.objectName(), QStringLiteral("FindBar"));
}

TEST(FindBarTest, SetsNoColoursOfItsOwn) {
    // Three themes ship as stylesheets; anything painted from a literal here
    // would be right in at most one of them.
    Harness h;
    EXPECT_TRUE(h.bar.styleSheet().isEmpty());
    const QList<QWidget *> children = h.bar.findChildren<QWidget *>();
    for (QWidget *child : children)
        EXPECT_TRUE(child->styleSheet().isEmpty()) << child->objectName().toStdString();
}

TEST(FindBarTest, EnterEmitsAForwardSearchForTheTypedText) {
    Harness h;
    h.bar.setQuery(QStringLiteral("MZ"));
    QTest::keyClick(h.input(), Qt::Key_Return);

    EXPECT_EQ(h.request.count, 1);
    EXPECT_EQ(h.request.direction, ByteSearch::Direction::Forward);
    EXPECT_TRUE(h.request.needle.valid);
    EXPECT_EQ(h.request.needle.bytes, QByteArrayLiteral("MZ"));
}

TEST(FindBarTest, ShiftEnterAndTheButtonsChooseTheDirection) {
    Harness h;
    h.bar.setQuery(QStringLiteral("MZ"));

    QTest::keyClick(h.input(), Qt::Key_Return, Qt::ShiftModifier);
    ASSERT_EQ(h.request.count, 1);
    EXPECT_EQ(h.request.direction, ByteSearch::Direction::Backward);

    h.button("FindBarNextButton")->click();
    ASSERT_EQ(h.request.count, 2);
    EXPECT_EQ(h.request.direction, ByteSearch::Direction::Forward);

    h.button("FindBarPrevButton")->click();
    ASSERT_EQ(h.request.count, 3);
    EXPECT_EQ(h.request.direction, ByteSearch::Direction::Backward);
}

TEST(FindBarTest, DoesNotSearchForAnEmptyQuery) {
    Harness h;
    QTest::keyClick(h.input(), Qt::Key_Return);
    EXPECT_EQ(h.request.count, 0);
    EXPECT_FALSE(h.button("FindBarNextButton")->isEnabled());
    EXPECT_FALSE(h.button("FindBarPrevButton")->isEnabled());
}

TEST(FindBarTest, HexModeCompilesBytesAndRetiresTheCaseSwitch) {
    Harness h;
    h.bar.setQuery(QStringLiteral("4D 5A"));
    // As text, "4D 5A" is five characters; as hex it is two bytes.
    EXPECT_EQ(h.bar.needle().bytes, QByteArrayLiteral("4D 5A"));

    h.bar.setMode(ByteSearch::Mode::Hex);
    EXPECT_EQ(h.bar.needle().bytes, QByteArray::fromHex("4d5a"));
    // A byte has no case, so offering the switch would be a lie.
    EXPECT_FALSE(h.button("FindBarCaseToggle")->isEnabled());

    h.bar.setMode(ByteSearch::Mode::Text);
    EXPECT_TRUE(h.button("FindBarCaseToggle")->isEnabled());
}

TEST(FindBarTest, ShowsTheParseErrorAndRefusesToSearchOnBadHex) {
    Harness h;
    h.bar.setMode(ByteSearch::Mode::Hex);
    h.bar.setQuery(QStringLiteral("4D 5"));

    EXPECT_FALSE(h.bar.needle().valid);
    QLabel *note = h.label("FindBarNote");
    EXPECT_TRUE(note->isVisible());
    EXPECT_EQ(note->text(), h.bar.needle().error);
    EXPECT_EQ(note->property("semanticState").toString(), QStringLiteral("error"));
    EXPECT_FALSE(h.button("FindBarNextButton")->isEnabled());

    QTest::keyClick(h.input(), Qt::Key_Return);
    EXPECT_EQ(h.request.count, 0);
}

TEST(FindBarTest, SurfacesTheCaseFoldingCaveatAsAWarningNotAnError) {
    Harness h;
    h.bar.setEncoding(QByteArrayLiteral("UTF-16"));
    h.bar.setCaseInsensitive(true);
    h.bar.setQuery(QStringLiteral("case"));

    const ByteSearch::Needle needle = h.bar.needle();
    ASSERT_TRUE(needle.valid); // still searchable, just not case-insensitively
    EXPECT_EQ(needle.folding, ByteSearch::CaseFolding::Exact);
    QLabel *note = h.label("FindBarNote");
    EXPECT_TRUE(note->isVisible());
    EXPECT_EQ(note->property("semanticState").toString(), QStringLiteral("warning"));
    EXPECT_TRUE(h.button("FindBarNextButton")->isEnabled());
}

TEST(FindBarTest, ReencodesTheNeedleWhenTheHostChangesEncoding) {
    Harness h;
    h.bar.setQuery(QStringLiteral("中"));
    EXPECT_EQ(h.bar.needle().bytes, QByteArray::fromHex("e4b8ad"));
    const int before = h.queryChanges;

    h.bar.setEncoding(QByteArrayLiteral("UTF-16"));
    EXPECT_EQ(h.bar.needle().bytes, QByteArray::fromHex("2d4e"));
    EXPECT_GT(h.queryChanges, before); // the host must be told to drop highlights
}

TEST(FindBarTest, ReportsTheHostsResultAndDropsItWhenTheQueryChanges) {
    Harness h;
    h.bar.setQuery(QStringLiteral("MZ"));
    QLabel *status = h.label("FindBarStatus");

    h.bar.showMatch(3, 12);
    EXPECT_EQ(status->text(), QStringLiteral("3 / 12"));
    EXPECT_EQ(status->property("semanticState").toString(), QStringLiteral("muted"));

    h.bar.showNoMatch();
    EXPECT_FALSE(status->text().isEmpty());
    EXPECT_EQ(status->property("semanticState").toString(), QStringLiteral("error"));

    // A count belonging to the previous needle must not survive an edit.
    h.bar.setQuery(QStringLiteral("MZX"));
    EXPECT_TRUE(status->text().isEmpty());
    EXPECT_FALSE(status->isVisible());
}

TEST(FindBarTest, EscapeAndTheCloseButtonHideTheBarAndTellTheHost) {
    Harness h;
    QTest::keyClick(h.input(), Qt::Key_Escape);
    EXPECT_EQ(h.closedCount, 1);
    EXPECT_FALSE(h.bar.isVisible());

    h.bar.activate();
    EXPECT_TRUE(h.bar.isVisible());
    h.button("FindBarCloseButton")->click();
    EXPECT_EQ(h.closedCount, 2);
    EXPECT_FALSE(h.bar.isVisible());
}

TEST(FindBarTest, ActivateFocusesAndSelectsWhatIsAlreadyThere) {
    // F3 on an open bar should let the next keystroke replace the old query
    // rather than append to it.
    Harness h;
    h.bar.setQuery(QStringLiteral("MZ"));
    h.bar.activate();
    EXPECT_TRUE(h.input()->hasSelectedText());
    EXPECT_EQ(h.input()->selectedText(), QStringLiteral("MZ"));
}
