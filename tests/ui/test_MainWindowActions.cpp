#include <gtest/gtest.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>

#include <clocale>

#include "ArchiveHandler.h"
#include "FilePanel.h"
#include "FileSystemModel.h"
#include "FunctionKeyBar.h"
#include "MainWindow.h"
#include "Settings.h"
#include "ThemeManager.h"
#include "TitleBar.h"
#include "TranslationManager.h"
#include "ThemeStateGuard.h"

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
            action->text() != QStringLiteral("Image Colours Follow Theme") &&
            action->text() != QStringLiteral("Preview Colours Follow Theme")) {
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

class EnvironmentGuard final {
public:
    EnvironmentGuard(const char *name, const QByteArray &value)
        : m_name(name), m_original(qgetenv(name)), m_hadOriginal(qEnvironmentVariableIsSet(name)) {
        qputenv(name, value);
    }
    ~EnvironmentGuard() {
        if (m_hadOriginal)
            qputenv(m_name.constData(), m_original);
        else
            qunsetenv(m_name.constData());
    }

private:
    QByteArray m_name;
    QByteArray m_original;
    bool m_hadOriginal;
};

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
    ThemeStateGuard themeState;
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    // Three menu buttons plus the account button, which shares the object name.
    EXPECT_EQ(window.findChildren<QToolButton *>(QStringLiteral("TitleMenuButton")).size(), 4);
    TitleBar *titleBar = window.findChild<TitleBar *>();
    ASSERT_NE(titleBar, nullptr);
    QStringList visibleMenuOrder;
    for (QToolButton *button : titleBar->findChildren<QToolButton *>(
             QStringLiteral("TitleMenuButton"), Qt::FindDirectChildrenOnly)) {
        if (QMenu *menu = button->menu();
            menu && (menu->title() == QStringLiteral("&Interface") ||
                     menu->title() == QStringLiteral("&Actions") ||
                     menu->title() == QStringLiteral("Con&fig"))) {
            visibleMenuOrder.append(menu->title());
        }
    }
    EXPECT_EQ(visibleMenuOrder,
              (QStringList{QStringLiteral("&Interface"), QStringLiteral("&Actions"),
                           QStringLiteral("Con&fig")}));
    for (const QString &title : {QStringLiteral("&Interface"), QStringLiteral("Con&fig"),
                                 QStringLiteral("&Actions")}) {
        QMenu *menu = findMenu(window, title);
        ASSERT_NE(menu, nullptr);
        EXPECT_TRUE(menu->actions().isEmpty()) << title.toStdString();
    }
    EXPECT_TRUE(window.findChildren<QWidgetAction *>().isEmpty());
}

TEST(MainWindowActionsTest, ActionsMenuAppendsToolsAfterTheStarCommands) {
    ThemeStateGuard themeState;
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    const QList<FilePanel *> panels = window.findChildren<FilePanel *>();
    ASSERT_GE(panels.size(), 2);
    FilePanel *left = panels.at(0);
    window.setActivePanel(left);

    QMenu *actionsMenu = findMenu(window, QStringLiteral("&Actions"));
    ASSERT_NE(actionsMenu, nullptr);
    // ... and it is on the menu bar, not merely parented to the window: the
    // title bar gives every menu it was handed a button of its own.
    bool onMenuBar = false;
    for (QToolButton *button :
         window.findChildren<QToolButton *>(QStringLiteral("TitleMenuButton")))
        onMenuBar = onMenuBar || button->menu() == actionsMenu;
    EXPECT_TRUE(onMenuBar);
    openMenu(actionsMenu);

    // The "✳" button pops exactly this list, so the two must agree entry for
    // entry. The popup builds its actions fresh on every click (each one is
    // bound to the panel it was opened from), so the labels are the strongest
    // comparison available -- there is no shared QAction instance to point at.
    QScopedPointer<QMenu> starMenu(window.buildShortcutMenu(left));
    ASSERT_FALSE(starMenu->actions().isEmpty());
    const QList<QAction *> starActions = starMenu->actions();
    const QList<QAction *> actions = actionsMenu->actions();
    ASSERT_EQ(actions.size(), starActions.size() + 5); // separator + four tools
    for (int i = 0; i < starActions.size(); ++i)
        EXPECT_EQ(actions.at(i)->text(), starActions.at(i)->text());
    EXPECT_TRUE(actions.at(starActions.size())->isSeparator());
    EXPECT_NE(findAction(actionsMenu, QStringLiteral("Cloud Clipboard")), nullptr);
    EXPECT_NE(findAction(actionsMenu, QStringLiteral("Calculate Checksums")), nullptr);
    EXPECT_NE(findAction(actionsMenu, QStringLiteral("Secure Wipe")), nullptr);
    EXPECT_NE(findAction(actionsMenu, QStringLiteral("Compare Files")), nullptr);
}

TEST(MainWindowActionsTest, ActionsMenuCommandsRunOnTheActivePanel) {
    ThemeStateGuard themeState;
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    const QList<FilePanel *> panels = window.findChildren<FilePanel *>();
    ASSERT_GE(panels.size(), 2);
    FilePanel *left = panels.at(0);
    FilePanel *right = panels.at(1);
    window.setActivePanel(right);

    QMenu *actionsMenu = findMenu(window, QStringLiteral("&Actions"));
    ASSERT_NE(actionsMenu, nullptr);
    openMenu(actionsMenu);

    const bool leftBefore = left->isThumbnailMode();
    const bool rightBefore = right->isThumbnailMode();
    QAction *toggle = findAction(actionsMenu, rightBefore ? QStringLiteral("Switch to List View")
                                                          : QStringLiteral("Switch to Thumbnail View"));
    ASSERT_NE(toggle, nullptr);
    toggle->trigger();
    qApp->processEvents();

    EXPECT_NE(right->isThumbnailMode(), rightBefore);
    EXPECT_EQ(left->isThumbnailMode(), leftBefore);
}

TEST(MainWindowActionsTest, ReplayedMenuMouseMoveDoesNotLeaveResizeCursor) {
    ThemeStateGuard themeState;
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

TEST(MainWindowActionsTest, QuickConnectClickClearsTopResizeCursor) {
    ThemeStateGuard themeState;
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;
    window.resize(900, 600);
    window.show();
    qApp->processEvents();

    FunctionKeyBar *functionKeyBar = window.findChild<FunctionKeyBar *>();
    ASSERT_NE(functionKeyBar, nullptr);
    QPushButton *quickConnectButton = nullptr;
    for (QPushButton *button : functionKeyBar->findChildren<QPushButton *>()) {
        if (button->toolTip() == QStringLiteral("Connect External / Devices")) {
            quickConnectButton = button;
            break;
        }
    }
    ASSERT_NE(quickConnectButton, nullptr);
    ASSERT_TRUE(quickConnectButton->isVisible());

    // Hover the exposed top resize band first, then click the actual quick-connect
    // launcher. The child click must clear the MainWindow's inherited resize cursor.
    const QPoint exposedTop(window.width() / 2, 15);
    ASSERT_EQ(window.childAt(exposedTop), nullptr);
    QMouseEvent edgeMove(QEvent::MouseMove, exposedTop, Qt::NoButton, Qt::NoButton,
                         Qt::NoModifier);
    QApplication::sendEvent(&window, &edgeMove);
    ASSERT_EQ(window.cursor().shape(), Qt::SizeVerCursor);

    QTest::mouseClick(quickConnectButton, Qt::LeftButton);
    qApp->processEvents();

    EXPECT_EQ(window.cursor().shape(), Qt::ArrowCursor);
}

TEST(MainWindowActionsTest, FirstOpenBuildsEachMenuOnceWithCurrentState) {
    ThemeStateGuard themeState;
    ScopedUiLanguage language(QStringLiteral("en"));
    MainWindow window;

    QMenu *actionsMenu = findMenu(window, QStringLiteral("&Actions"));
    QMenu *configMenu = findMenu(window, QStringLiteral("Con&fig"));
    QMenu *interfaceMenu = findMenu(window, QStringLiteral("&Interface"));
    ASSERT_NE(actionsMenu, nullptr);
    ASSERT_NE(configMenu, nullptr);
    ASSERT_NE(interfaceMenu, nullptr);
    EXPECT_TRUE(actionsMenu->actions().isEmpty());
    EXPECT_TRUE(configMenu->actions().isEmpty());
    EXPECT_TRUE(interfaceMenu->actions().isEmpty());

    openMenu(actionsMenu);
    QAction *notepad = findAction(actionsMenu, QStringLiteral("Cloud Clipboard"));
    ASSERT_NE(notepad, nullptr);
    EXPECT_TRUE(notepad->isEnabled());
    const int actionsCount = actionsMenu->actions().size();
    openMenu(actionsMenu);
    EXPECT_EQ(actionsMenu->actions().size(), actionsCount);

    openMenu(configMenu);
    QAction *noConfirm = findAction(configMenu, QStringLiteral("Skip Trash Delete Confirmation"));
    ASSERT_NE(noConfirm, nullptr);
    EXPECT_TRUE(noConfirm->isCheckable());
    EXPECT_EQ(noConfirm->toolTip(),
              QStringLiteral("Skip confirmation only when deleting local files to the trash. "
                             "Shift+Delete and remote deletes always require confirmation."));
    ASSERT_FALSE(configMenu->actions().isEmpty());
    // Trailing "\tCtrl+Alt+P" stripped the same way findAction() does: this entry
    // carries a shortcut, unlike the retired one that used to sit last here.
    EXPECT_EQ(configMenu->actions().last()->text().section(QLatin1Char('\t'), 0, 0),
              QStringLiteral("Show System Partitions"));
    // The entry that used to sit last is still built -- the assertion above is
    // about the menu being complete, not about which entry happens to end it.
    EXPECT_NE(findAction(configMenu, QStringLiteral("Check for Updates")), nullptr);
    EXPECT_EQ(findAction(configMenu, QStringLiteral("Automatic Update Check")), nullptr);
    EXPECT_EQ(findAction(configMenu, QStringLiteral("FileCommander Account")), nullptr);
    TitleBar *titleBar = window.findChild<TitleBar *>();
    ASSERT_NE(titleBar, nullptr);
    QToolButton *accountButton = nullptr;
    for (QToolButton *button : titleBar->findChildren<QToolButton *>()) {
        if (button->text() == QStringLiteral("Account")) {
            accountButton = button;
            break;
        }
    }
    ASSERT_NE(accountButton, nullptr);
    ASSERT_NE(accountButton->menu(), nullptr);
    EXPECT_NE(findAction(accountButton->menu(), QStringLiteral("Sign In")), nullptr);
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
    ScopedUiLanguage language(QStringLiteral("zh_CN"));
    MainWindow window;

    QMenu *actionsMenu = findMenu(window, QStringLiteral("操作(&A)"));
    QMenu *configMenu = findMenu(window, QStringLiteral("配置(&F)"));
    QMenu *interfaceMenu = findMenu(window, QStringLiteral("界面(&I)"));
    openMenu(actionsMenu);
    openMenu(configMenu);
    openMenu(interfaceMenu);

    EXPECT_NE(findAction(actionsMenu, QStringLiteral("云剪贴板")), nullptr);
    EXPECT_NE(findAction(interfaceMenu, QStringLiteral("显示功能键栏")), nullptr);
}

TEST(MainWindowActionsTest, SkipTrashDeleteConfirmationActionExplainsItsSafetyBoundary) {
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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

// The entry said "Directly Open Archives" while ticking it set
// archiveAsFolder=false -- it turned archive browsing OFF. Anyone who ticked it
// to get into archives got the opposite of what it promised.
TEST(MainWindowActionsTest, TheArchiveEntryIsTickedWhenArchivesOpenAsFolders) {
    ThemeStateGuard themeState;
    std::setlocale(LC_NUMERIC, "C");
    ScopedUiLanguage language(QStringLiteral("en"));
    QTemporaryDir configDir;
    ASSERT_TRUE(configDir.isValid());
    EnvironmentGuard config("FILECOMMANDER_CONFIG_HOME", configDir.path().toUtf8());

    {
        Settings settings;
        settings.setArchiveAsFolder(true);
    }

    MainWindow window;
    QMenu *configMenu = findMenu(window, QStringLiteral("Con&fig"));
    ASSERT_NE(configMenu, nullptr);
    openMenu(configMenu);

    QAction *action = window.findChild<QAction *>(QStringLiteral("configDirectArchivesAction"));
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isChecked())
        << "browsing is on, so the entry that offers it must be ticked";

    // And toggling it has to move the setting the same way, not the opposite.
    action->trigger();
    EXPECT_FALSE(Settings().archiveAsFolder());
    action->trigger();
    EXPECT_TRUE(Settings().archiveAsFolder());
}

// Extraction used to run entirely inside the menu handler, so on a network
// path -- where every read crosses the wire -- the window froze with no bar and
// no way out. It now runs on a worker thread and reports back, which this
// pins down from the outside: right after the command returns, nothing has
// been unpacked yet, and the files only appear once the event loop runs.
TEST(MainWindowActionsTest, ExtractingAnArchiveDoesNotRunOnTheGuiThread) {
    ThemeStateGuard themeState;
    std::setlocale(LC_NUMERIC, "C");

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString payload = QDir(dir.path()).filePath(QStringLiteral("hello.txt"));
    {
        QFile file(payload);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        // Incompressible, and big enough that unpacking it cannot finish in the
        // handful of microseconds between the command returning and the check
        // below -- which is what makes that check a fact rather than a race.
        QByteArray blob(8 << 20, Qt::Uninitialized);
        for (int i = 0; i < blob.size(); ++i)
            blob[i] = char(i * 2654435761u >> 13);
        file.write(blob);
    }
    const QString archive = QDir(dir.path()).filePath(QStringLiteral("fixture.zip"));
    QString createError;
    ASSERT_TRUE(ArchiveHandler::create(archive, {payload}, QStringLiteral("zip"), &createError))
        << createError.toStdString();
    ASSERT_TRUE(QFile::remove(payload)); // so its reappearance means the extraction ran

#ifdef Q_OS_WIN
    // The Windows Debug CRT can assert while this async window tears down; keep
    // it off the top-level list until the runner's process exit.
    auto *windowHost = new QWidget;
    auto *window = new MainWindow(windowHost);
#else
    MainWindow windowStorage;
    auto *window = &windowStorage;
#endif
    FilePanel *panel = window->findChildren<FilePanel *>().value(0);
    ASSERT_NE(panel, nullptr);
    window->setActivePanel(panel);
    panel->navigateTo(dir.path());

    int row = -1;
    ASSERT_TRUE(QTest::qWaitFor([panel, &row] {
        for (int r = 0; r < panel->model()->rowCount(); ++r) {
            if (!panel->model()->isParentEntry(r) &&
                panel->model()->fileInfoAt(r).name() == QStringLiteral("fixture.zip")) {
                row = r;
                return true;
            }
        }
        return false;
    }, 10000)) << "the panel never listed the archive";
    panel->view()->setCurrentIndex(panel->model()->index(row, 0));

    // The command ends in a modal report, and with no user here nothing else
    // would close it.
    QTimer dismisser;
    QObject::connect(&dismisser, &QTimer::timeout, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    dismisser.start(100);

    ASSERT_TRUE(QMetaObject::invokeMethod(window, "extractArchiveHere", Qt::DirectConnection));
    // The whole point: the handler returned with the work still outstanding.
    const QString extracted =
        QDir(dir.path()).filePath(QStringLiteral("fixture/hello.txt"));
    EXPECT_FALSE(QFile::exists(extracted) || QFile::exists(payload))
        << "extraction finished inside the handler, so it is still blocking the GUI thread";

    EXPECT_TRUE(QTest::qWaitFor(
        [&extracted, &payload] { return QFile::exists(extracted) || QFile::exists(payload); },
        30000))
        << "the archive was never unpacked";

    // ...and the panel showing that directory re-lists itself, so the user sees
    // what came out without pressing refresh. Which of the two names appears
    // depends on whether smartExtract wrapped the single entry in a folder.
    EXPECT_TRUE(QTest::qWaitFor([panel] {
        for (int r = 0; r < panel->model()->rowCount(); ++r) {
            const QString name = panel->model()->fileInfoAt(r).name();
            if (!panel->model()->isParentEntry(r) &&
                (name == QStringLiteral("fixture") || name == QStringLiteral("hello.txt")))
                return true;
        }
        return false;
    }, 10000)) << "the panel never picked up the extracted files";
}
