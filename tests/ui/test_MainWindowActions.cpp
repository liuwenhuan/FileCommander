#include <gtest/gtest.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QShortcut>
#include <QToolButton>
#include <QWidgetAction>

#include <clocale>

#include "FilePanel.h"
#include "FunctionKeyBar.h"
#include "MainWindow.h"
#include "Settings.h"
#include "ThemeManager.h"
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

QShortcut *findShortcut(MainWindow &window, const QKeySequence &key) {
    for (QShortcut *shortcut : window.findChildren<QShortcut *>()) {
        if (shortcut->key() == key)
            return shortcut;
    }
    return nullptr;
}

void activateShortcut(MainWindow &window, const QKeySequence &key) {
    QShortcut *shortcut = findShortcut(window, key);
    ASSERT_NE(shortcut, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection));
}

QAction *checkedThemeAction(QMenu *interfaceMenu) {
    QAction *themeAction = findAction(interfaceMenu, QStringLiteral("&Theme"));
    if (!themeAction)
        return nullptr;
    QMenu *themeMenu = themeAction->menu();
    if (!themeMenu)
        return nullptr;
    for (QAction *action : themeMenu->actions()) {
        if (action->isCheckable() && action->isChecked() &&
            action->text() != QStringLiteral("Tint images to match")) {
            return action;
        }
    }
    return nullptr;
}

Settings::Theme themeForAction(const QAction *action) {
    if (action->text() == QStringLiteral("Auto"))
        return Settings::Theme::Auto;
    if (action->text() == QStringLiteral("Light"))
        return Settings::Theme::Light;
    if (action->text() == QStringLiteral("Dark"))
        return Settings::Theme::Dark;
    return Settings::Theme::Crt;
}

QAction *anotherThemeAction(QMenu *interfaceMenu, QAction *first, QAction *second) {
    for (const QString &label : {QStringLiteral("Auto"), QStringLiteral("Light"),
                                 QStringLiteral("Dark"), QStringLiteral("Green CRT")}) {
        QAction *action = findAction(interfaceMenu, label);
        if (action && action != first && action != second)
            return action;
    }
    return nullptr;
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

TEST(MainWindowActionsTest, ReplayedMenuMouseMoveDoesNotLeaveResizeCursor) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;
    window.resize(900, 600);
    window.show();
    qApp->processEvents();

    QToolButton *menuButton =
        window.findChild<QToolButton *>(QStringLiteral("TitleMenuButton"));
    ASSERT_NE(menuButton, nullptr);
    ASSERT_TRUE(menuButton->isVisible());

    // A popup menu can replay its closing mouse event to the parent window.
    // The upper part of this button overlaps the frameless resize grab band,
    // but it is occupied title-bar chrome rather than an exposed window edge.
    const QPoint buttonTop = menuButton->mapTo(&window, QPoint(menuButton->width() / 2, 0));
    const QPoint exposedEdge(buttonTop.x(), buttonTop.y() - 1);
    ASSERT_EQ(window.childAt(exposedEdge), nullptr);
    QMouseEvent edgeMove(QEvent::MouseMove, exposedEdge, Qt::NoButton, Qt::NoButton,
                         Qt::NoModifier);
    QApplication::sendEvent(&window, &edgeMove);
    EXPECT_EQ(window.cursor().shape(), Qt::SizeVerCursor);

    const QPoint windowPos = buttonTop + QPoint(0, 1);
    ASSERT_NE(window.childAt(windowPos), nullptr);
    QMouseEvent replayedMove(QEvent::MouseMove, windowPos, Qt::NoButton, Qt::NoButton,
                             Qt::NoModifier);
    QApplication::sendEvent(&window, &replayedMove);

    EXPECT_NE(window.cursor().shape(), Qt::SizeVerCursor);
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
    // Trailing "\tCtrl+Alt+U" stripped the same way findAction() does: this entry
    // carries a shortcut, unlike the retired one that used to sit last here.
    EXPECT_EQ(configMenu->actions().last()->text().section(QLatin1Char('\t'), 0, 0),
              QStringLiteral("Automatic Update Check"));
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

TEST(MainWindowActionsTest, ConfigMenuRefreshesDeleteConfirmationAfterRuntimeShortcut) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *configMenu = findMenu(window, QStringLiteral("Con&fig"));
    ASSERT_NE(configMenu, nullptr);
    openMenu(configMenu);
    QAction *noConfirm = findAction(configMenu, QStringLiteral("Skip Trash Delete Confirmation"));
    ASSERT_NE(noConfirm, nullptr);
    const bool initiallyChecked = noConfirm->isChecked();
    const int actionCount = configMenu->actions().size();

    activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_D));
    openMenu(configMenu);

    EXPECT_EQ(noConfirm->isChecked(), !initiallyChecked);
    EXPECT_EQ(configMenu->actions().size(), actionCount);

    activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_D));
}

TEST(MainWindowActionsTest, InterfaceMenuRefreshesFunctionKeyBarAfterRuntimeShortcut) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *interfaceMenu = findMenu(window, QStringLiteral("&Interface"));
    ASSERT_NE(interfaceMenu, nullptr);
    openMenu(interfaceMenu);
    QAction *showFunctions = findAction(interfaceMenu, QStringLiteral("Show Function Key Bar"));
    FunctionKeyBar *functionKeyBar = window.findChild<FunctionKeyBar *>();
    ASSERT_NE(showFunctions, nullptr);
    ASSERT_NE(functionKeyBar, nullptr);
    const int actionCount = interfaceMenu->actions().size();

    activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J));
    openMenu(interfaceMenu);

    EXPECT_EQ(showFunctions->isChecked(), !functionKeyBar->isHidden());
    EXPECT_EQ(interfaceMenu->actions().size(), actionCount);

    activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J));
}

TEST(MainWindowActionsTest, InterfaceMenuRefreshesThemeAfterRuntimeShortcut) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *interfaceMenu = findMenu(window, QStringLiteral("&Interface"));
    ASSERT_NE(interfaceMenu, nullptr);
    openMenu(interfaceMenu);
    QAction *initialTheme = checkedThemeAction(interfaceMenu);
    ASSERT_NE(initialTheme, nullptr);
    const int actionCount = interfaceMenu->actions().size();

    activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T));
    openMenu(interfaceMenu);

    EXPECT_NE(checkedThemeAction(interfaceMenu), initialTheme);
    EXPECT_EQ(interfaceMenu->actions().size(), actionCount);

    for (int i = 0; i < 3; ++i)
        activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T));
}

TEST(MainWindowActionsTest, InterfaceThemeGroupRemainsExclusiveAfterRuntimeSync) {
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *interfaceMenu = findMenu(window, QStringLiteral("&Interface"));
    ASSERT_NE(interfaceMenu, nullptr);
    openMenu(interfaceMenu);
    QAction *initialTheme = checkedThemeAction(interfaceMenu);
    ASSERT_NE(initialTheme, nullptr);
    QActionGroup *themeGroup = initialTheme->actionGroup();
    ASSERT_NE(themeGroup, nullptr);
    const int actionCount = interfaceMenu->actions().size();

    activateShortcut(window, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T));
    openMenu(interfaceMenu);
    QAction *runtimeTheme = checkedThemeAction(interfaceMenu);
    ASSERT_NE(runtimeTheme, nullptr);
    ASSERT_NE(runtimeTheme, initialTheme);
    QAction *selectedTheme = anotherThemeAction(interfaceMenu, initialTheme, runtimeTheme);
    ASSERT_NE(selectedTheme, nullptr);
    const Settings::Theme expectedTheme = themeForAction(selectedTheme);

    selectedTheme->trigger();

    int checkedThemes = 0;
    for (QAction *action : themeGroup->actions())
        checkedThemes += action->isChecked();
    EXPECT_EQ(checkedThemes, 1);
    EXPECT_EQ(themeGroup->checkedAction(), selectedTheme);
    EXPECT_EQ(checkedThemeAction(interfaceMenu), selectedTheme);
    EXPECT_EQ(Settings().theme(), expectedTheme);
    ThemeManager *themeManager = window.findChild<ThemeManager *>();
    ASSERT_NE(themeManager, nullptr);
    EXPECT_EQ(themeManager->requestedTheme(), expectedTheme);
    EXPECT_EQ(interfaceMenu->actions().size(), actionCount);

    initialTheme->trigger();
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

// The "✳" button belongs to one panel, so its menu must act on that panel --
// even when the other one is active. Closing the popup restores keyboard focus
// to the previously focused view, which re-activates the other panel before the
// action's triggered() arrives, so the panel the button emitted panelActivated()
// for is no longer the active one by the time the command runs.
TEST(MainWindowActionsTest, PanelShortcutMenuActsOnItsOwnPanelNotTheActiveOne) {
    std::setlocale(LC_NUMERIC, "C");
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;
    window.resize(1000, 700);

    const QList<FilePanel *> panels = window.findChildren<FilePanel *>();
    ASSERT_GE(panels.size(), 2);
    FilePanel *left = panels.at(0);
    FilePanel *right = panels.at(1);
    ASSERT_FALSE(left->isThumbnailMode());
    ASSERT_FALSE(right->isThumbnailMode());

    // Left is active; the menu is the RIGHT panel's.
    window.setActivePanel(left);
    QScopedPointer<QMenu> menu(window.buildShortcutMenu(right));
    ASSERT_FALSE(menu.isNull());
    QAction *toggle = findAction(menu.data(), QStringLiteral("Switch to Thumbnail View"));
    ASSERT_NE(toggle, nullptr);

    // Simulate the popup handing focus back to the active panel's view, exactly
    // as QMenu does on close, before the action is delivered.
    window.setActivePanel(left);
    toggle->trigger();

    EXPECT_TRUE(right->isThumbnailMode());
    EXPECT_FALSE(left->isThumbnailMode());
}
