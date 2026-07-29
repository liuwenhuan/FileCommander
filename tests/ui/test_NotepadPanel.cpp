#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>

#include "NotepadPanel.h"
#include "config/Settings.h"
#include "notepad/NotepadStore.h"

namespace {

void addNotes(QPushButton *button, int count) {
    for (int i = 0; i < count; ++i)
        button->click();
    QCoreApplication::processEvents();
}

} // namespace

TEST(NotepadPanelTest, SavesNotesUsingInjectedSettingsAndDirectory) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString notesPath = temporaryDir.filePath(QStringLiteral("panel-notes"));
    Settings settings(temporaryDir.filePath(QStringLiteral("panel.ini")));
    NotepadPanel panel(settings, notesPath);

    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("NotepadEditor"));
    ASSERT_NE(editor, nullptr);
    editor->setPlainText(QString::fromUtf8("隔离笔记\nwith Unicode"));
    panel.saveAll();

    NotepadStore reloadedStore(notesPath);
    ASSERT_EQ(reloadedStore.notes().size(), 1);
    EXPECT_EQ(reloadedStore.load(reloadedStore.notes().first().id),
              QString::fromUtf8("隔离笔记\nwith Unicode"));
    EXPECT_TRUE(QDir(notesPath).exists());
}

TEST(NotepadPanelTest, FailedSaveKeepsEditorContentAndBlocksNoteSwitch) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString notesPath = temporaryDir.filePath(QStringLiteral("failed-save-notes"));
    NotepadStore fixture(notesPath);
    const NotepadNote first = fixture.create(QStringLiteral("first"));
    const NotepadNote second = fixture.create(QStringLiteral("second"));
    ASSERT_FALSE(first.id.isEmpty());
    ASSERT_FALSE(second.id.isEmpty());

    Settings settings(temporaryDir.filePath(QStringLiteral("failed-save.ini")));
    NotepadPanel panel(settings, notesPath);
    auto *editor = panel.findChild<QPlainTextEdit *>(QStringLiteral("NotepadEditor"));
    auto *list = panel.findChild<QListWidget *>(QStringLiteral("NotepadList"));
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);

    editor->setPlainText(QStringLiteral("unsaved content"));
    ASSERT_TRUE(QFile::remove(first.filePath));
    ASSERT_TRUE(QDir().mkpath(first.filePath));

    list->setCurrentRow(1);

    EXPECT_EQ(list->currentRow(), 0);
    EXPECT_EQ(editor->toPlainText(), QStringLiteral("unsaved content"));
    EXPECT_TRUE(editor->isEnabled());
}

TEST(NotepadPanelTest, DraggingDividerGrowsUpwardAndKeepsBottomAtAnchor) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    Settings settings(temporaryDir.filePath(QStringLiteral("divider.ini")));
    settings.setNotepadEditorHeight(120);
    NotepadPanel panel(settings, temporaryDir.filePath(QStringLiteral("divider-notes")));
    const QRect appContent(100, 100, 700, 700);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    auto *button = panel.findChild<QPushButton *>(QStringLiteral("NotepadNewButton"));
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
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    const QString notesPath = temporaryDir.filePath(QStringLiteral("cap-notes"));
    NotepadStore store(notesPath);
    for (int i = 0; i < 31; ++i)
        store.create(QStringLiteral("Note %1").arg(i));

    Settings settings(temporaryDir.filePath(QStringLiteral("cap.ini")));
    settings.setNotepadEditorHeight(120);
    NotepadPanel panel(settings, notesPath);
    const QRect appContent(100, 410, 700, 350);
    const QRect anchor(700, 760, 40, 30);
    panel.popUpAbove(anchor, appContent);
    QCoreApplication::processEvents();

    auto *list = panel.findChild<QListWidget *>(QStringLiteral("NotepadList"));
    ASSERT_NE(list, nullptr);
    EXPECT_LE(panel.height(), anchor.top() - appContent.top());
    EXPECT_GE(panel.geometry().top(), appContent.top());
    EXPECT_GT(list->verticalScrollBar()->maximum(), 0)
        << "once the popup is capped, only the note list should absorb overflow by scrolling";
}

TEST(NotepadPanelTest, DynamicResizeNeverEscapesItsAppContentOrScreenCap) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());

    Settings settings(temporaryDir.filePath(QStringLiteral("resize.ini")));
    settings.setNotepadEditorHeight(120);
    NotepadPanel panel(settings, temporaryDir.filePath(QStringLiteral("resize-notes")));
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
