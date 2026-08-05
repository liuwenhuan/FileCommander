#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QFileIconProvider>
#include <QImage>
#include <QTemporaryDir>

#include "IconCache.h"
#include "theme/Phosphor.h"
#include "MainWindow.h"
#include "ThemeManager.h"
#include "OpenWithHandlers.h"
#include "ThemeStateGuard.h"

namespace {

QStringList entryTexts(QMenu *menu) {
    QStringList texts;
    for (QAction *action : menu->actions()) {
        if (action->isSeparator())
            texts << QStringLiteral("---");
        else if (action->menu())
            texts << action->text() + QStringLiteral(" >");
        else
            texts << action->text();
    }
    return texts;
}

QMenu *subMenuNamed(QMenu *menu, const QString &text) {
    for (QAction *action : menu->actions())
        if (action->menu() && action->text() == text)
            return action->menu();
    return nullptr;
}

} // namespace


// Every test here constructs a MainWindow, whose default constructor reads the
// REAL user settings and applies the theme they name -- installing an
// APPLICATION stylesheet (16841 characters of it on this machine, measured).
// Left in place it changes how every later test resolves fonts: QStyleSheetStyle
// assigns a font to a widget at polish time, so a view stops propagating one to
// its viewport the way it does with no sheet. That is what silently broke
// FilePanelFontTest.CombinedTypographyAppliesFamilyAndSizeOnceToTheDetailsSurface
// in every full-suite run, while it passed alone.
//
// The application object outlives each test, so whatever a test changes on it
// has to be put back.
class OpenWithMenuTest : public ::testing::Test {
protected:
    // See ThemeStateGuard.h: these tests construct a MainWindow, whose default
    // constructor applies the user's real theme, and one of them applies
    // another on purpose.
    ThemeStateGuard themeState;
};

// "Open With" used to be a single prompt asking the user to type a command.
// It now lists what the system says can open this type, then the rest of what
// is installed, and keeps the file dialog as the last resort.
TEST_F(OpenWithMenuTest, ListsRegisteredApplicationsAndKeepsTheFileDialog) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("note.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello");
    file.close();

    MainWindow window;
    QScopedPointer<QMenu> menu(window.buildOpenWithMenu(path));
    ASSERT_FALSE(menu.isNull());
    const QStringList texts = entryTexts(menu.data());

    // The escape hatch is always there, whatever the system does or does not
    // register -- it is the one entry that never depends on the registry.
    bool hasBrowse = false;
    for (const QString &text : texts)
        if (text.contains(QStringLiteral("Choose Another Application")))
            hasBrowse = true;
    EXPECT_TRUE(hasBrowse) << texts.join(QStringLiteral(" | ")).toStdString();

    const auto handlers = fc::openWithHandlers(path);
    if (handlers.isEmpty())
        GTEST_SKIP() << "this system registers nothing for .txt";

    int recommended = 0;
    for (const fc::OpenWithHandler &handler : handlers)
        if (handler.recommended)
            ++recommended;

    // Every application registered for the type is offered directly, not
    // hidden a level down: those are the ones the user came for.
    for (const fc::OpenWithHandler &handler : handlers) {
        if (!handler.recommended)
            continue;
        EXPECT_TRUE(texts.contains(handler.displayName))
            << handler.displayName.toStdString() << " is missing from "
            << texts.join(QStringLiteral(" | ")).toStdString();
    }

    // ...and the rest are reachable rather than dropped.
    if (recommended > 0 && recommended < handlers.size()) {
        QMenu *rest = subMenuNamed(menu.data(), QStringLiteral("Other Applications"));
        ASSERT_NE(rest, nullptr) << texts.join(QStringLiteral(" | ")).toStdString();
        const QStringList restTexts = entryTexts(rest);
        for (const fc::OpenWithHandler &handler : handlers) {
            if (handler.recommended)
                continue;
            EXPECT_TRUE(restTexts.contains(handler.displayName))
                << handler.displayName.toStdString() << " is offered nowhere";
        }
    }
}

// With nothing registered for the type, burying the installed applications one
// level down would leave a menu that looks empty. They move up instead.
TEST_F(OpenWithMenuTest, AnUnregisteredTypeStillListsWhatIsInstalled) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("data.zqxj"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    const auto handlers = fc::openWithHandlers(path);
    int recommended = 0;
    for (const fc::OpenWithHandler &handler : handlers)
        if (handler.recommended)
            ++recommended;
    if (handlers.isEmpty() || recommended > 0)
        GTEST_SKIP() << "this system does register something for .zqxj";

    MainWindow window;
    QScopedPointer<QMenu> menu(window.buildOpenWithMenu(path));
    EXPECT_EQ(subMenuNamed(menu.data(), QStringLiteral("Other Applications")), nullptr)
        << "with no recommended list to separate them from, the applications "
           "belong at the top level";
    const QStringList texts = entryTexts(menu.data());
    for (const fc::OpenWithHandler &handler : handlers)
        EXPECT_TRUE(texts.contains(handler.displayName))
            << handler.displayName.toStdString() << " is offered nowhere";
}

// A menu built for a file with no type to look up still has to be usable.
TEST_F(OpenWithMenuTest, AnExtensionlessFileStillOffersTheFileDialog) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("plain"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    MainWindow window;
    QScopedPointer<QMenu> menu(window.buildOpenWithMenu(path));
    ASSERT_FALSE(menu.isNull());
    EXPECT_FALSE(menu->actions().isEmpty());
    EXPECT_TRUE(entryTexts(menu.data()).last().contains(QStringLiteral("Choose Another Application")));
}

namespace {

QImage iconImage(const QIcon &icon) {
    return icon.isNull() ? QImage() : icon.pixmap(32, 32).toImage();
}

} // namespace

// The applications' icons come from the system, and the system draws them in
// their own colours. Every other icon in the window answers to the theme, so
// taken straight from QFileIconProvider these sat in full colour in a menu of
// phosphor-green glyphs.
TEST_F(OpenWithMenuTest, ApplicationIconsAreTintedLikeEverythingElse) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("note.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello");
    file.close();

    QString program;
    for (const fc::OpenWithHandler &handler : fc::openWithHandlers(path)) {
        if (!handler.program.isEmpty() && QFileInfo::exists(handler.program)) {
            program = handler.program;
            break;
        }
    }
    if (program.isEmpty())
        GTEST_SKIP() << "no application with an executable is registered for .txt";

    MainWindow window;
    ThemeManager *themes = window.findChild<ThemeManager *>();
    ASSERT_NE(themes, nullptr);
    // The theme where tinting file artwork IS the theme.
    themes->apply(Settings::Theme::Crt, true, true);

    QScopedPointer<QMenu> menu(window.buildOpenWithMenu(path));
    ASSERT_FALSE(menu.isNull());

    // The cache answers only once the shell has been asked, which happens on a
    // worker; the actions pick the icons up when it lands. Wait for THAT, not
    // for the cache -- the worker warms every program before it reports back.
    const auto firstIconAction = [&menu]() -> QAction * {
        for (QAction *action : menu->actions()) {
            if (QMenu *sub = action->menu()) {
                for (QAction *inner : sub->actions())
                    if (!inner->icon().isNull())
                        return inner;
                continue;
            }
            if (!action->icon().isNull())
                return action;
        }
        return nullptr;
    };
    QElapsedTimer timer;
    timer.start();
    QAction *found = nullptr;
    while (timer.elapsed() < 10000 && !found) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        found = firstIconAction();
    }
    const QIcon themed = IconCache::instance().systemIconForPath(program);
    ASSERT_FALSE(themed.isNull()) << "the icon warm-up never produced " << program.toStdString();

    ASSERT_NE(found, nullptr) << "no menu entry carries an icon at all";

    // The tint has to have been applied: the same executable through the raw
    // provider is what this used to show, and it must not look like that.
    const QImage untinted = iconImage(QFileIconProvider().icon(QFileInfo(program)));
    const QImage shown = iconImage(found->icon());
    ASSERT_FALSE(shown.isNull());
    ASSERT_FALSE(untinted.isNull());
    EXPECT_NE(shown, untinted) << "the menu is showing the system's own colours";
    EXPECT_EQ(shown, iconImage(themed)) << "the menu is not using the themed cache";

    // No apply() to some other theme here: that would leave a DIFFERENT sheet
    // behind rather than none. Restore above puts back exactly what was there.
}
