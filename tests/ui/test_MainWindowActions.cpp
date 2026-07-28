#include <gtest/gtest.h>

#include <QAction>
#include <QMenu>

#include <clocale>

#include "MainWindow.h"

namespace {

QAction *findAction(QMenu *menu, const QString &text) {
    if (!menu)
        return nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text().section(QLatin1Char('\t'), 0, 0) == text)
            return action;
        if (QMenu *subMenu = action->menu()) {
            if (QAction *found = findAction(subMenu, text))
                return found;
        }
    }
    return nullptr;
}

} // namespace

TEST(MainWindowActionsTest, SkipTrashDeleteConfirmationActionExplainsItsSafetyBoundary) {
    std::setlocale(LC_NUMERIC, "C");
    MainWindow window;

    QMenu *configMenu = nullptr;
    for (QMenu *menu : window.findChildren<QMenu *>()) {
        if (menu->title() == QStringLiteral("Con&fig")) {
            configMenu = menu;
            break;
        }
    }
    ASSERT_NE(configMenu, nullptr);

    QAction *action = findAction(configMenu, QStringLiteral("Skip Trash Delete Confirmation"));
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isCheckable());
    EXPECT_EQ(action->toolTip(),
              QStringLiteral("Skip confirmation only when deleting local files to the trash. "
                             "Shift+Delete and remote deletes always require confirmation."));
}
