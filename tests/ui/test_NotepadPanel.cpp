#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QUuid>

#include "NotepadPanel.h"
#include "config/Settings.h"

namespace {

class IsolatedNotepadStore {
public:
    IsolatedNotepadStore() {
        m_parent = Settings::configDir();
        m_backupName = QStringLiteral("notepad-test-backup-") +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir parent(m_parent);
        if (parent.exists(QStringLiteral("notepad")))
            m_saved = parent.rename(QStringLiteral("notepad"), m_backupName);
        else
            m_saved = true;
        m_ready = m_saved && parent.mkpath(QStringLiteral("notepad"));
    }

    ~IsolatedNotepadStore() {
        if (!m_ready)
            return;
        QDir(m_parent + QStringLiteral("/notepad")).removeRecursively();
        if (m_saved)
            QDir(m_parent).rename(m_backupName, QStringLiteral("notepad"));
    }

    bool isReady() const { return m_ready; }

private:
    QString m_parent;
    QString m_backupName;
    bool m_saved = false;
    bool m_ready = false;
};

class NotepadEditorHeightGuard {
public:
    NotepadEditorHeightGuard() : m_old(Settings().notepadEditorHeight()) {
        Settings().setNotepadEditorHeight(120);
    }

    ~NotepadEditorHeightGuard() { Settings().setNotepadEditorHeight(m_old); }

private:
    int m_old;
};

QPushButton *newButton(NotepadPanel &panel) {
    for (QPushButton *button : panel.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("New"))
            return button;
    }
    return nullptr;
}

void addNotes(QPushButton *button, int count) {
    for (int i = 0; i < count; ++i)
        button->click();
    QCoreApplication::processEvents();
}

} // namespace

TEST(NotepadPanelTest, DraggingDividerGrowsUpwardAndKeepsBottomAtAnchor) {
    IsolatedNotepadStore store;
    ASSERT_TRUE(store.isReady());
    NotepadEditorHeightGuard settings;
    NotepadPanel panel;
    const QRect appContent(100, 100, 700, 700);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    QPushButton *button = newButton(panel);
    ASSERT_NE(button, nullptr);
    addNotes(button, 6);

    auto *splitter = panel.findChild<QSplitter *>(QStringLiteral("NotepadSplitter"));
    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("NotepadEditor"));
    ASSERT_NE(splitter, nullptr);
    ASSERT_NE(editor, nullptr);

    const int maxHeight = anchor.top() - appContent.top();
    const int initialHeight = panel.height();
    ASSERT_LT(initialHeight, maxHeight) << "the isolated fixture must leave headroom to grow";

    const QList<int> initialSizes = splitter->sizes();
    ASSERT_EQ(initialSizes.size(), 2);
    const int draggedPosition = qMax(0, initialSizes.at(0) - 40);
    splitter->setSizes({draggedPosition, initialSizes.at(1) + 40});

    // QSplitter updates sizes before emitting this signal for a genuine drag.
    // Drive that public signal directly: unlike synthesized pointer input this
    // behaves identically under both Xvfb and Qt's offscreen platform plugin.
    QSignalSpy moved(splitter, &QSplitter::splitterMoved);
    ASSERT_TRUE(QMetaObject::invokeMethod(splitter, "splitterMoved", Qt::DirectConnection,
                                          Q_ARG(int, draggedPosition), Q_ARG(int, 1)));
    QCoreApplication::processEvents();

    EXPECT_EQ(moved.count(), 1) << "applyDynamicSize must not re-emit splitterMoved";
    EXPECT_GT(panel.height(), initialHeight)
        << "a larger editor must enlarge the fly-out instead of only stealing list space";
    EXPECT_LE(qAbs(panel.geometry().bottom() - anchor.top()), 2)
        << "the fly-out must grow upward while its bottom remains attached to the launcher";
    EXPECT_GE(editor->height(), 120);
}

TEST(NotepadPanelTest, AtHeightCapTheNoteListScrollsInsteadOfEscapingTheContentArea) {
    IsolatedNotepadStore store;
    ASSERT_TRUE(store.isReady());
    NotepadEditorHeightGuard settings;
    NotepadPanel panel;
    const QRect appContent(100, 410, 700, 350);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    QPushButton *button = newButton(panel);
    ASSERT_NE(button, nullptr);
    addNotes(button, 30);

    auto *list = panel.findChild<QListWidget *>(QStringLiteral("NotepadList"));
    ASSERT_NE(list, nullptr);
    EXPECT_LE(panel.height(), anchor.top() - appContent.top());
    EXPECT_GE(panel.geometry().top(), appContent.top());
    EXPECT_GT(list->verticalScrollBar()->maximum(), 0)
        << "once the popup is capped, only the note list should absorb overflow by scrolling";
}

TEST(NotepadPanelTest, DynamicResizeNeverEscapesItsAppContentOrScreenCap) {
    IsolatedNotepadStore store;
    ASSERT_TRUE(store.isReady());
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
