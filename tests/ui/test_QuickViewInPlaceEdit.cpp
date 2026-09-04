#include <gtest/gtest.h>

#include <QAction>
#include <QFile>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
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

QAction *actionWithText(QObject *owner, const QString &text) {
    for (QAction *action : owner->findChildren<QAction *>()) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

QString writeLongLines(const QTemporaryDir &dir, int count) {
    const QString path = dir.filePath(QStringLiteral("wrapped.txt"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&file);
    for (int i = 0; i < count; ++i)
        out << "line " << i << ' ' << QString(400, QLatin1Char('x')) << '\n';
    return path;
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
    const int previewPosition = text->cursorForPosition(text->viewport()->rect().topLeft()).position();

    QSignalSpy editing(&view, &QuickView::editingChanged);
    ASSERT_TRUE(view.beginEditing(path));
    EXPECT_TRUE(view.isEditing());
    auto *editor = view.findChild<TextEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_FALSE(editor->isWindow()) << "the editor popped up as its own window";
    ASSERT_EQ(editing.size(), 1);
    EXPECT_TRUE(editing.at(0).at(0).toBool());
    FC_TRY_COMPARE_WITH_TIMEOUT(editor->codeEditor()->textCursor().position(), previewPosition, 5000);

    editor->codeEditor()->verticalScrollBar()->setValue(300);
    const int editorPosition =
        editor->codeEditor()->cursorForPosition(editor->codeEditor()->viewport()->rect().topLeft()).position();
    emit editor->previewRequested(path); // what the toolbar's Preview button does
    EXPECT_FALSE(view.isEditing());
    FC_TRY_COMPARE_WITH_TIMEOUT(text->textCursor().position(), editorPosition, 5000);
}

TEST(QuickViewInPlaceEdit, WrapStaysSynchronizedAndKeepsLogicalTextPosition) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeLongLines(dir, 180);
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded);
    view.resize(420, 300);
    view.show();
    view.showFile(path);

    QPlainTextEdit *text = textView(view);
    ASSERT_NE(text, nullptr);
    FC_TRY_VERIFY_WITH_TIMEOUT(text->document()->blockCount() >= 180, 5000);
    QAction *previewWrap = actionWithText(&view, QStringLiteral("Wrap"));
    ASSERT_NE(previewWrap, nullptr);
    EXPECT_FALSE(previewWrap->isChecked());
    EXPECT_EQ(text->lineWrapMode(), QPlainTextEdit::NoWrap);

    previewWrap->setChecked(true);
    EXPECT_EQ(text->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    QTextCursor previewCursor(text->document()->findBlockByNumber(70));
    text->setTextCursor(previewCursor);
    text->centerCursor();
    const int previewPosition = text->cursorForPosition(text->viewport()->rect().topLeft()).position();

    ASSERT_TRUE(view.beginEditing(path));
    auto *editor = view.findChild<TextEditor *>();
    ASSERT_NE(editor, nullptr);
    QAction *editorWrap = actionWithText(editor, QStringLiteral("Wrap"));
    ASSERT_NE(editorWrap, nullptr);
    EXPECT_TRUE(editorWrap->isChecked());
    EXPECT_EQ(editor->codeEditor()->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    FC_TRY_COMPARE_WITH_TIMEOUT(editor->codeEditor()->textCursor().position(), previewPosition, 5000);

    editorWrap->setChecked(false);
    EXPECT_EQ(text->lineWrapMode(), QPlainTextEdit::NoWrap);
    QTextCursor editorCursor(editor->codeEditor()->document()->findBlockByNumber(130));
    editor->codeEditor()->setTextCursor(editorCursor);
    editor->codeEditor()->centerCursor();
    const int editorPosition = editor->codeEditor()
                                   ->cursorForPosition(editor->codeEditor()->viewport()->rect().topLeft())
                                   .position();
    emit editor->previewRequested(path);

    FC_TRY_VERIFY_WITH_TIMEOUT(!view.isEditing(), 5000);
    FC_TRY_VERIFY_WITH_TIMEOUT(text->lineWrapMode() == QPlainTextEdit::NoWrap, 5000);
    FC_TRY_COMPARE_WITH_TIMEOUT(text->textCursor().position(), editorPosition, 5000);
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

TEST(QuickViewInPlaceEdit, SwitchingEditingFileSavesAndKeepsTheEditorPage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("first.txt"));
    const QString second = dir.filePath(QStringLiteral("second.txt"));
    for (const auto &entry : {qMakePair(first, QByteArray("first\n")),
                              qMakePair(second, QByteArray("second\n"))}) {
        QFile file(entry.first);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        ASSERT_EQ(file.write(entry.second), entry.second.size());
    }

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Embedded);
    ASSERT_TRUE(view.beginEditing(first));
    TextEditor *editor = view.findChild<TextEditor *>();
    ASSERT_NE(editor, nullptr);
    editor->codeEditor()->moveCursor(QTextCursor::End);
    editor->codeEditor()->insertPlainText(QStringLiteral("saved before switch\n"));
    ASSERT_TRUE(editor->isDocumentModified());

    ASSERT_TRUE(view.switchEditingFile(second));
    EXPECT_TRUE(view.isEditing());
    EXPECT_EQ(view.findChild<TextEditor *>(), editor);
    EXPECT_EQ(editor->filePath(), second);
    EXPECT_FALSE(editor->isDocumentModified());

    QFile saved(first);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_TRUE(saved.readAll().contains("saved before switch"));
    EXPECT_TRUE(editor->codeEditor()->toPlainText().contains(QStringLiteral("second")));
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
