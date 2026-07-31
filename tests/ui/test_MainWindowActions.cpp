#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QToolButton>
#include <QWidgetAction>

#include <clocale>

#include "FunctionKeyBar.h"
#include "MainWindow.h"
#include "TranslationManager.h"

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

QMenu *findMenu(MainWindow &window, const QString &title) {
    for (QMenu *menu : window.findChildren<QMenu *>()) {
        if (menu->title() == title)
            return menu;
    }
    return nullptr;
}

void openMenu(QMenu *menu) {
    ASSERT_NE(menu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
}

class ScopedUiLanguage final {
public:
    explicit ScopedUiLanguage(const QString &language) {
        TranslationManager::switchTo(*qApp, language);
        qApp->processEvents();
    }

    ~ScopedUiLanguage() {
        TranslationManager::switchTo(*qApp, QStringLiteral("en"));
        qApp->processEvents();
    }
};

} // namespace

TEST(MainWindowActionsTest, StartupKeepsMenuButtonsButDefersMenuContents) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    EXPECT_EQ(window.findChildren<QToolButton *>(QStringLiteral("TitleMenuButton")).size(), 3);
    for (const QString &title : {QStringLiteral("&Interface"), QStringLiteral("&Tools"),
                                 QStringLiteral("Con&fig")}) {
        QMenu *menu = findMenu(window, title);
        ASSERT_NE(menu, nullptr);
        EXPECT_TRUE(menu->actions().isEmpty()) << title.toStdString();
    }
    EXPECT_TRUE(window.findChildren<QWidgetAction *>().isEmpty());
}

TEST(MainWindowActionsTest, FirstOpenBuildsEachMenuOnceWithCurrentState) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *toolsMenu = findMenu(window, QStringLiteral("&Tools"));
    QMenu *configMenu = findMenu(window, QStringLiteral("Con&fig"));
    QMenu *interfaceMenu = findMenu(window, QStringLiteral("&Interface"));
    ASSERT_NE(toolsMenu, nullptr);
    ASSERT_NE(configMenu, nullptr);
    ASSERT_NE(interfaceMenu, nullptr);
    EXPECT_TRUE(toolsMenu->actions().isEmpty());
    EXPECT_TRUE(configMenu->actions().isEmpty());
    EXPECT_TRUE(interfaceMenu->actions().isEmpty());

    openMenu(toolsMenu);
    QAction *notepad = findAction(toolsMenu, QStringLiteral("Quick Notepad"));
    ASSERT_NE(notepad, nullptr);
    EXPECT_TRUE(notepad->isEnabled());
    const int toolsActionCount = toolsMenu->actions().size();
    openMenu(toolsMenu);
    EXPECT_EQ(toolsMenu->actions().size(), toolsActionCount);

    openMenu(configMenu);
    QAction *noConfirm = findAction(configMenu, QStringLiteral("Skip Trash Delete Confirmation"));
    ASSERT_NE(noConfirm, nullptr);
    EXPECT_TRUE(noConfirm->isCheckable());
    EXPECT_EQ(noConfirm->toolTip(),
              QStringLiteral("Skip confirmation only when deleting local files to the trash. "
                             "Shift+Delete and remote deletes always require confirmation."));
    ASSERT_FALSE(configMenu->actions().isEmpty());
    EXPECT_EQ(configMenu->actions().last()->text(),
              QStringLiteral("Associate Folder Open Actions"));
    const int configActionCount = configMenu->actions().size();
    openMenu(configMenu);
    EXPECT_EQ(configMenu->actions().size(), configActionCount);

    openMenu(interfaceMenu);
    EXPECT_NE(findAction(interfaceMenu, QStringLiteral("Choose Font")), nullptr);
    QAction *showFunctions = findAction(interfaceMenu, QStringLiteral("Show Function Key Bar"));
    ASSERT_NE(showFunctions, nullptr);
    EXPECT_TRUE(showFunctions->isCheckable());
    FunctionKeyBar *functionKeyBar = window.findChild<FunctionKeyBar *>();
    ASSERT_NE(functionKeyBar, nullptr);
    EXPECT_EQ(showFunctions->isChecked(), !functionKeyBar->isHidden());
    EXPECT_EQ(interfaceMenu->findChildren<QWidgetAction *>().size(), 2);
    const int interfaceActionCount = interfaceMenu->actions().size();
    openMenu(interfaceMenu);
    EXPECT_EQ(interfaceMenu->actions().size(), interfaceActionCount);
    EXPECT_EQ(interfaceMenu->findChildren<QWidgetAction *>().size(), 2);
}

TEST(MainWindowActionsTest, FirstOpenBuildsTranslatedMenuContents) {
    ScopedUiLanguage language(QStringLiteral("zh_CN"));
    MainWindow window;

    QMenu *toolsMenu = findMenu(window, QStringLiteral("工具(&T)"));
    QMenu *configMenu = findMenu(window, QStringLiteral("配置(&F)"));
    QMenu *interfaceMenu = findMenu(window, QStringLiteral("界面(&I)"));
    openMenu(toolsMenu);
    openMenu(configMenu);
    openMenu(interfaceMenu);

    EXPECT_NE(findAction(toolsMenu, QStringLiteral("快捷记事本")), nullptr);
    EXPECT_NE(findAction(configMenu, QStringLiteral("删除时无需确认")), nullptr);
    EXPECT_NE(findAction(interfaceMenu, QStringLiteral("显示功能键栏")), nullptr);
}

TEST(MainWindowActionsTest, SkipTrashDeleteConfirmationActionExplainsItsSafetyBoundary) {
    std::setlocale(LC_NUMERIC, "C");
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *configMenu = findMenu(window, QStringLiteral("Con&fig"));
    ASSERT_NE(configMenu, nullptr);
    openMenu(configMenu);

    QAction *action = findAction(configMenu, QStringLiteral("Skip Trash Delete Confirmation"));
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isCheckable());
    EXPECT_EQ(action->toolTip(),
              QStringLiteral("Skip confirmation only when deleting local files to the trash. "
                             "Shift+Delete and remote deletes always require confirmation."));
}
