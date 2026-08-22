#include <gtest/gtest.h>

#include <QFile>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTextCursor>

#include "QuickView.h"
#include "TextEditor.h"
#include "TryUntil.h"
#include "config/Settings.h"

// The preview and the editor are two pages of ONE QuickView: pressing Edit must
// not open a window, and a switch in either direction must land on the line the
// other side was showing.

namespace {

QString writeNumberedLines(const QTemporaryDir &dir, int count) {
    const QString path = dir.filePath(QStringLiteral("long.txt"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&file);
    for (int i = 0; i < count; ++i)
        out << "line " << i << "\n";
    return path;
}

QPlainTextEdit *textView(QuickView &view) {
    return view.findChild<QPlainTextEdit *>(QStringLiteral("quickViewTextView"));
}

} // namespace

TEST(QuickViewInPlaceEdit, EditAndPreviewStayInOneWidgetAndKeepTheScrollPosition) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeNumberedLines(dir, 500);
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    view.resize(600, 400);
    view.show();

    view.showFile(path);
    QPlainTextEdit *text = textView(view);
    ASSERT_NE(text, nullptr);
    // The text load is a background probe; nothing is on screen until it lands.
    FC_TRY_VERIFY_WITH_TIMEOUT(text->verticalScrollBar()->maximum() > 200, 5000);
    text->verticalScrollBar()->setValue(120);

    QSignalSpy editing(&view, &QuickView::editingChanged);
    ASSERT_TRUE(view.beginEditing(path));
    EXPECT_TRUE(view.isEditing());
    auto *editor = view.findChild<TextEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_FALSE(editor->isWindow()) << "the editor popped up as its own window";
    ASSERT_EQ(editing.size(), 1);
    EXPECT_TRUE(editing.at(0).at(0).toBool());
    FC_TRY_COMPARE_WITH_TIMEOUT(editor->codeEditor()->verticalScrollBar()->value(), 120, 5000);

    editor->codeEditor()->verticalScrollBar()->setValue(300);
    emit editor->previewRequested(path); // what the toolbar's Preview button does
    EXPECT_FALSE(view.isEditing());
    FC_TRY_COMPARE_WITH_TIMEOUT(text->verticalScrollBar()->value(), 300, 5000);
}

TEST(QuickViewInPlaceEdit, AnUnsavedBufferSurvivesTheFileCursorMovingOn) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeNumberedLines(dir, 20);
    ASSERT_FALSE(path.isEmpty());
    const QString other = dir.filePath(QStringLiteral("other.txt"));
    QFile file(other);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("elsewhere\n");
    file.close();

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    ASSERT_TRUE(view.beginEditing(path));
    auto *editor = view.findChild<TextEditor *>();
    ASSERT_NE(editor, nullptr);
    editor->codeEditor()->insertPlainText(QStringLiteral("dirty"));
    ASSERT_TRUE(editor->isDocumentModified());

    view.showFile(other);
    EXPECT_TRUE(view.isEditing()) << "the unsaved buffer was thrown away";
}

TEST(QuickViewInPlaceEdit, SameFilePreviewFlushesPendingAutosaveBeforeLeavingEditor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeNumberedLines(dir, 20);
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    ASSERT_TRUE(view.beginEditing(path));
    auto *editor = view.findChild<TextEditor *>();
    ASSERT_NE(editor, nullptr);
    editor->codeEditor()->moveCursor(QTextCursor::End);
    editor->codeEditor()->insertPlainText(QStringLiteral("autosaved now\n"));
    ASSERT_TRUE(editor->isDocumentModified());

    // This is the transition Preview ultimately requests. It happens before the
    // 600 ms timer has elapsed, so showFile() must synchronously flush first.
    view.showFile(path);
    EXPECT_FALSE(view.isEditing());
    QFile saved(path);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray savedBytes = saved.readAll();
    EXPECT_TRUE(savedBytes.contains("autosaved now")) << savedBytes.toHex().constData();

    QPlainTextEdit *text = textView(view);
    ASSERT_NE(text, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(text->toPlainText().endsWith(QStringLiteral("autosaved now\n")),
                               5000);
}
