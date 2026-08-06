#include <gtest/gtest.h>

#include <QKeySequence>
#include <QWidget>

#include "CommandRegistry.h"

// The rules that used to live in comments around five parallel QMaps inside
// MainWindow, and which nothing checked: extracting the registry is what made
// them testable, and writing these found that none of them had ever been
// covered -- deleting the label fallback left all 712 ui tests passing.
namespace {

std::function<void()> counting(int *calls) {
    return [calls]() { ++*calls; };
}

} // namespace

TEST(CommandRegistryTest, RunsTheHandlerRegisteredForAnId) {
    QWidget parent;
    CommandRegistry registry(&parent);
    int calls = 0;
    registry.bind(QStringLiteral("copy"), QStringLiteral("Copy"), QKeySequence(Qt::Key_F5),
                  QKeySequence(Qt::Key_F5), counting(&calls));

    EXPECT_TRUE(registry.run(QStringLiteral("copy")));
    EXPECT_EQ(calls, 1);
}

TEST(CommandRegistryTest, RunningAnUnknownIdSaysSoRatherThanDoingNothingQuietly) {
    QWidget parent;
    CommandRegistry registry(&parent);
    // The distinction matters: a caller handing over a stale id from settings
    // needs to be able to tell it apart from a command that ran and had nothing
    // to do.
    EXPECT_FALSE(registry.run(QStringLiteral("no-such-command")));
}

TEST(CommandRegistryTest, RebindingRefreshesTheLabelWithoutBindingTheKeyTwice) {
    QWidget parent;
    CommandRegistry registry(&parent);
    int first = 0;
    int second = 0;
    registry.bind(QStringLiteral("copy"), QStringLiteral("Copy"), QKeySequence(Qt::Key_F5),
                  QKeySequence(Qt::Key_F5), counting(&first));
    // What a language change does: same ids, translated labels.
    registry.bind(QStringLiteral("copy"), QStringLiteral("复制"), QKeySequence(Qt::Key_F5),
                  QKeySequence(Qt::Key_F5), counting(&second));

    EXPECT_EQ(registry.label(QStringLiteral("copy")).toStdString(), "复制");

    // The second handler is discarded rather than added: a second QShortcut on
    // the same key would fire the command twice per press.
    EXPECT_EQ(registry.keyedOrder().size(), 1);
    EXPECT_EQ(registry.keyedOrder().first().second.toStdString(), "复制");
    registry.run(QStringLiteral("copy"));
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0);
}

TEST(CommandRegistryTest, AnUnknownLabelFallsBackToTheIdRatherThanToNothing) {
    QWidget parent;
    CommandRegistry registry(&parent);
    // A function-key slot pointing at a command that no longer exists -- from
    // an older version's settings -- must still put something on the button.
    EXPECT_EQ(registry.label(QStringLiteral("retired-command")).toStdString(), "retired-command");
}

TEST(CommandRegistryTest, TheLiveKeyWinsOverTheRecordedDefault) {
    QWidget parent;
    CommandRegistry registry(&parent);
    registry.bind(QStringLiteral("copy"), QStringLiteral("Copy"), QKeySequence(Qt::Key_F5),
                  QKeySequence(Qt::CTRL | Qt::Key_C), {});

    // Bound with the user's key, not the default: the menus must show what
    // pressing the key would actually do.
    EXPECT_EQ(registry.sequence(QStringLiteral("copy")), QKeySequence(Qt::CTRL | Qt::Key_C));
    EXPECT_EQ(registry.defaults().value(QStringLiteral("copy")), QKeySequence(Qt::Key_F5));

    registry.setSequence(QStringLiteral("copy"), QKeySequence(Qt::Key_F6));
    EXPECT_EQ(registry.sequence(QStringLiteral("copy")), QKeySequence(Qt::Key_F6));
}

TEST(CommandRegistryTest, ACommandWithNoKeyStillCarriesADefaultToShow) {
    QWidget parent;
    CommandRegistry registry(&parent);
    // F3-F8 are reassignable slots, so the command itself owns no QShortcut --
    // but the change dialog still shows which key it is the default for.
    registry.setDefaultSequence(QStringLiteral("view"), QKeySequence(Qt::Key_F3));
    EXPECT_EQ(registry.sequence(QStringLiteral("view")), QKeySequence(Qt::Key_F3));
}

TEST(CommandRegistryTest, SettingTheKeyOfAnIdThatHasNoneIsIgnored) {
    QWidget parent;
    CommandRegistry registry(&parent);
    registry.registerCommand(QStringLiteral("about"), QStringLiteral("About"), {});
    // The shortcuts dialog returns a map; an entry for a keyless command must
    // not invent a shortcut for it.
    registry.setSequence(QStringLiteral("about"), QKeySequence(Qt::Key_F9));
    EXPECT_TRUE(registry.currentSequences().isEmpty());
}

TEST(CommandRegistryTest, RegisterCommandKeepsTheHandlerItAlreadyHad) {
    QWidget parent;
    CommandRegistry registry(&parent);
    int first = 0;
    int second = 0;
    registry.registerCommand(QStringLiteral("about"), QStringLiteral("About"), counting(&first));
    registry.registerCommand(QStringLiteral("about"), QStringLiteral("关于"), counting(&second));

    EXPECT_EQ(registry.label(QStringLiteral("about")).toStdString(), "关于");
    registry.run(QStringLiteral("about"));
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0);
}

TEST(CommandRegistryTest, KeyedCommandsAreListedInRegistrationOrder) {
    QWidget parent;
    CommandRegistry registry(&parent);
    registry.bind(QStringLiteral("view"), QStringLiteral("View"), QKeySequence(Qt::Key_F3),
                  QKeySequence(Qt::Key_F3), {});
    registry.bind(QStringLiteral("edit"), QStringLiteral("Edit"), QKeySequence(Qt::Key_F4),
                  QKeySequence(Qt::Key_F4), {});
    registry.registerCommand(QStringLiteral("about"), QStringLiteral("About"), {});

    // Registration order, which is the order the shortcuts dialog lists -- not
    // alphabetical, and without the keyless command.
    const auto order = registry.keyedOrder();
    ASSERT_EQ(order.size(), 2);
    EXPECT_EQ(order.at(0).first.toStdString(), "view");
    EXPECT_EQ(order.at(1).first.toStdString(), "edit");

    // The picker offers every command, keyed or not.
    EXPECT_TRUE(registry.ids().contains(QStringLiteral("about")));
}
