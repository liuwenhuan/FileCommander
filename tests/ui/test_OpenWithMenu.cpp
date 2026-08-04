#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QTemporaryDir>

#include "MainWindow.h"
#include "OpenWithHandlers.h"

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

// "Open With" used to be a single prompt asking the user to type a command.
// It now lists what the system says can open this type, then the rest of what
// is installed, and keeps the file dialog as the last resort.
TEST(OpenWithMenu, ListsRegisteredApplicationsAndKeepsTheFileDialog) {
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
TEST(OpenWithMenu, AnUnregisteredTypeStillListsWhatIsInstalled) {
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
TEST(OpenWithMenu, AnExtensionlessFileStillOffersTheFileDialog) {
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
