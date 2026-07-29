#include <gtest/gtest.h>

#include <QApplication>
#include <QShortcut>
#include <QSplitter>

#include <clocale>

#include "FilePanel.h"
#include "MainWindow.h"
#include "QuickView.h"
#include "config/Settings.h"

namespace {

QSplitter *panelSplitter(MainWindow &window) {
    for (QSplitter *splitter : window.findChildren<QSplitter *>()) {
        if (splitter->orientation() != Qt::Horizontal || splitter->count() != 2)
            continue;
        if (qobject_cast<FilePanel *>(splitter->widget(0)) ||
            qobject_cast<FilePanel *>(splitter->widget(1)) ||
            qobject_cast<QuickView *>(splitter->widget(0)) ||
            qobject_cast<QuickView *>(splitter->widget(1)))
            return splitter;
    }
    return nullptr;
}

void processGuiEvents() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1000);
}

} // namespace

TEST(MainWindowPreviewSwapTest, CtrlUSwapsPreviewWithVisiblePanelAndKeepsHiddenPanelParked) {
    std::setlocale(LC_NUMERIC, "C");
    Settings settings;
    const QKeySequence previousSwap =
        settings.shortcut(QStringLiteral("swapPanels"), QKeySequence(Qt::CTRL | Qt::Key_U));
    settings.setShortcut(QStringLiteral("swapPanels"), QKeySequence(Qt::CTRL | Qt::Key_U));
    struct ShortcutRestore {
        QKeySequence previous;
        ~ShortcutRestore() { Settings().setShortcut(QStringLiteral("swapPanels"), previous); }
    } restore{previousSwap};

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    processGuiEvents();

    bool ctrlUBound = false;
    for (QShortcut *shortcut : window.findChildren<QShortcut *>()) {
        if (shortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_U)) {
            ctrlUBound = true;
            break;
        }
    }
    ASSERT_TRUE(ctrlUBound) << "Ctrl+U must remain bound to the panel swap command";

    QSplitter *splitter = panelSplitter(window);
    ASSERT_NE(splitter, nullptr);
    auto *left = qobject_cast<FilePanel *>(splitter->widget(0));
    auto *right = qobject_cast<FilePanel *>(splitter->widget(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();
    ASSERT_EQ(splitter->widget(0), left);
    ASSERT_NE(qobject_cast<QuickView *>(splitter->widget(1)), nullptr);
    EXPECT_EQ(right->parentWidget(), nullptr) << "inactive panel must be parked";

    left->view()->setFocus();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "swapPanels"));
    processGuiEvents();

    EXPECT_NE(qobject_cast<QuickView *>(splitter->widget(0)), nullptr);
    EXPECT_EQ(splitter->widget(1), left);
    EXPECT_EQ(right->parentWidget(), nullptr) << "Ctrl+U must not reveal the hidden panel";
    EXPECT_EQ(window.focusWidget(), left->activeView())
        << "visible file panel remains the window's keyboard target after the swap";

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "toggleQuickView"));
    processGuiEvents();

    EXPECT_EQ(splitter->widget(0), right) << "closing restores the parked panel to preview's new slot";
    EXPECT_EQ(splitter->widget(1), left) << "closing keeps the swapped panel position";
    EXPECT_EQ(window.focusWidget(), left->activeView());
}
