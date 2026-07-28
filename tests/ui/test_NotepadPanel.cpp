#include <gtest/gtest.h>

#include <QApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTest>

#include "NotepadPanel.h"
#include "config/Settings.h"

namespace {

class NotepadEditorHeightGuard {
public:
    NotepadEditorHeightGuard() : m_old(Settings().notepadEditorHeight()) {
        Settings().setNotepadEditorHeight(120);
    }

    ~NotepadEditorHeightGuard() { Settings().setNotepadEditorHeight(m_old); }

private:
    int m_old;
};

} // namespace

TEST(NotepadPanelTest, DraggingDividerGrowsUpwardAndKeepsBottomAtAnchor) {
    NotepadEditorHeightGuard settings;
    NotepadPanel panel;
    const QRect appContent(100, 100, 700, 700);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    QPushButton *newButton = nullptr;
    for (QPushButton *button : panel.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("New")) {
            newButton = button;
            break;
        }
    }
    ASSERT_NE(newButton, nullptr);
    for (int i = 0; i < 6; ++i)
        newButton->click();
    QCoreApplication::processEvents();

    auto *splitter = panel.findChild<QSplitter *>(QStringLiteral("NotepadSplitter"));
    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("NotepadEditor"));
    ASSERT_NE(splitter, nullptr);
    ASSERT_NE(editor, nullptr);

    const int initialHeight = panel.height();
    const QList<int> initialSizes = splitter->sizes();
    ASSERT_EQ(initialSizes.size(), 2);

    // Emulate a real drag: QSplitter changes its sizes before emitting
    // splitterMoved. Invoking the signal makes this deterministic on Xvfb.
    splitter->setSizes({qMax(0, initialSizes.at(0) - 40), initialSizes.at(1) + 40});
    ASSERT_TRUE(QMetaObject::invokeMethod(splitter, "splitterMoved", Qt::DirectConnection,
                                          Q_ARG(int, qMax(0, initialSizes.at(0) - 40)),
                                          Q_ARG(int, 1)));
    QCoreApplication::processEvents();

    EXPECT_GT(panel.height(), initialHeight)
        << "a larger editor must enlarge the fly-out instead of only stealing list space";
    EXPECT_LE(qAbs(panel.geometry().bottom() - anchor.top()), 2)
        << "the fly-out must grow upward while its bottom remains attached to the launcher";
    EXPECT_GE(editor->height(), 120);
}

TEST(NotepadPanelTest, DynamicResizeNeverEscapesItsAppContentOrScreenCap) {
    NotepadEditorHeightGuard settings;
    NotepadPanel panel;
    // Deliberately less than the preferred content height: this is the case that
    // used to be overruled by the 180px artificial lower cap.
    const QRect appContent(100, 700, 700, 90);
    const QRect anchor(700, 790, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    const QRect geometry = panel.geometry();
    EXPECT_GE(geometry.top(), appContent.top());
    EXPECT_LE(geometry.bottom(), anchor.top() + 2);

    QScreen *screen = QGuiApplication::screenAt(anchor.center());
    if (screen)
        EXPECT_TRUE(screen->availableGeometry().contains(geometry));
}
