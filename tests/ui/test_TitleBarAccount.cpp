#include <gtest/gtest.h>

#include <QMenu>
#include <QSignalSpy>
#include <QToolButton>
#include <QWidget>

#include "TitleBar.h"

namespace {

// The account entry is the last QToolButton in the bar: the menu buttons come
// first, then the stretch, then this one, then the window buttons (which are
// TitleButtons, not QToolButtons).
QToolButton *accountButton(TitleBar &bar) {
    const QList<QToolButton *> buttons = bar.findChildren<QToolButton *>();
    return buttons.isEmpty() ? nullptr : buttons.last();
}

} // namespace

TEST(TitleBarAccountTest, SignedOutOffersSignInAndHidesSignOut) {
    QWidget window;
    QMenu interfaceMenu(QStringLiteral("Interface"));
    TitleBar bar(&window, {&interfaceMenu}, &window);

    QToolButton *account = accountButton(bar);
    ASSERT_NE(account, nullptr);
    ASSERT_NE(account->menu(), nullptr);
    EXPECT_EQ(account->text(), TitleBar::tr("Account"));

    const QList<QAction *> actions = account->menu()->actions();
    ASSERT_EQ(actions.size(), 2);
    EXPECT_TRUE(actions.at(0)->isVisible());
    EXPECT_FALSE(actions.at(1)->isVisible()); // sign out, no session to end

    QSignalSpy requested(&bar, &TitleBar::accountRequested);
    actions.at(0)->trigger();
    EXPECT_EQ(requested.count(), 1);
}

TEST(TitleBarAccountTest, SignedInShowsTheNameAndOffersSignOut) {
    QWidget window;
    TitleBar bar(&window, {}, &window);

    bar.setAccountName(QStringLiteral("user@example.com"));
    QToolButton *account = accountButton(bar);
    ASSERT_NE(account, nullptr);
    EXPECT_EQ(account->text(), QStringLiteral("user@example.com"));

    const QList<QAction *> actions = account->menu()->actions();
    ASSERT_EQ(actions.size(), 2);
    EXPECT_TRUE(actions.at(1)->isVisible());

    QSignalSpy signedOut(&bar, &TitleBar::signOutRequested);
    actions.at(1)->trigger();
    EXPECT_EQ(signedOut.count(), 1);

    // Signing out puts the label back to the generic entry.
    bar.setAccountName(QString());
    EXPECT_EQ(account->text(), TitleBar::tr("Account"));
    EXPECT_FALSE(actions.at(1)->isVisible());
}
