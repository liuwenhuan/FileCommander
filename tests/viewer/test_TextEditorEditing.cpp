#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QTextCursor>
#include <QToolBar>
#include <QVBoxLayout>

#include "TextEditor.h"

namespace {

QString writeBytes(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    file.close();
    return path;
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

int encodingIndex(const TextEditor &editor, const QString &label) {
    return editor.encodingCombo()->findText(label);
}

// Types at the end of the buffer the way a user would. setPlainText() is not a
// substitute: it replaces the document wholesale and leaves it UNmodified, so a
// test built on it would never exercise the dirty state at all.
void typeAtEnd(TextEditor &editor, const QString &text) {
    QTextCursor cursor(editor.codeEditor()->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
}

} // namespace

TEST(TextEditorEditingTest, NothingReachesDiskUntilSaveIsPressed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray original = "alpha\nbeta\n";
    const QString path = writeBytes(dir, QStringLiteral("save.txt"), original);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    EXPECT_FALSE(editor.saveAction()->isEnabled());

    typeAtEnd(editor, QStringLiteral("gamma\n"));
    qApp->processEvents();

    // The whole point of the Save button: the buffer is dirty and the file is not.
    EXPECT_EQ(readBytes(path), original);
    EXPECT_TRUE(editor.codeEditor()->document()->isModified());
    EXPECT_TRUE(editor.saveAction()->isEnabled());
    EXPECT_TRUE(editor.windowTitle().endsWith(QLatin1Char('*')));

    editor.saveAction()->trigger();
    qApp->processEvents();

    EXPECT_EQ(readBytes(path), QByteArray("alpha\nbeta\ngamma\n"));
    EXPECT_FALSE(editor.codeEditor()->document()->isModified());
    EXPECT_FALSE(editor.saveAction()->isEnabled());
    EXPECT_FALSE(editor.windowTitle().endsWith(QLatin1Char('*')));
}

TEST(TextEditorEditingTest, ModifiedStateIsVisibleInTheToolbar) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeBytes(dir, QStringLiteral("dirty.txt"), "one\n");

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    auto *label = editor.toolBar()->findChild<QLabel *>(QStringLiteral("textEditorModified"));
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().isEmpty());

    typeAtEnd(editor, QStringLiteral("two\n"));
    qApp->processEvents();
    EXPECT_FALSE(label->text().isEmpty());

    ASSERT_TRUE(editor.save());
    qApp->processEvents();
    EXPECT_TRUE(label->text().isEmpty());
}

TEST(TextEditorEditingTest, AutoDetectsTheEncodingOnOpen) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTextCodec *gbk = QTextCodec::codecForName("GB18030");
    ASSERT_NE(gbk, nullptr);
    const QString chinese = QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95");
    const QString path = writeBytes(dir, QStringLiteral("gbk.txt"), gbk->fromUnicode(chinese));

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));

    EXPECT_EQ(editor.encodingCombo()->currentIndex(), 0); // Auto
    EXPECT_TRUE(editor.encodingStatusText().startsWith(QStringLiteral("Auto (")))
        << qPrintable(editor.encodingStatusText());
    EXPECT_EQ(editor.codeEditor()->toPlainText(), chinese);
}

TEST(TextEditorEditingTest, PureAsciiIsNeverGuessedAsUtf16) {
    // Eight ASCII bytes pair into four perfectly valid UTF-16 code units, which
    // once outscored the detector's ASCII answer. Here that would show the file
    // as CJK mojibake and then SAVE it as UTF-16, so the editor used to overrule
    // the detector; the detector now prefers ASCII itself and this pins that the
    // editor still gets the safe answer through it.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray ascii("one\ntwo\n"); // even length, all bytes < 0x80
    const QString path = writeBytes(dir, QStringLiteral("ascii.txt"), ascii);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    EXPECT_EQ(editor.codeEditor()->toPlainText(), QStringLiteral("one\ntwo\n"));
    EXPECT_EQ(editor.currentCodecName(), QByteArrayLiteral("UTF-8"));
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(readBytes(path), ascii);
}

TEST(TextEditorEditingTest, ChangingEncodingReDecodesTheBytesRatherThanTheBuffer) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTextCodec *gbk = QTextCodec::codecForName("GB18030");
    ASSERT_NE(gbk, nullptr);
    const QString chinese = QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95");
    const QString path = writeBytes(dir, QStringLiteral("recode.txt"), gbk->fromUnicode(chinese));

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_EQ(editor.codeEditor()->toPlainText(), chinese);

    const int latin1 = encodingIndex(editor, QStringLiteral("ISO-8859-1"));
    ASSERT_GE(latin1, 0);
    editor.encodingCombo()->setCurrentIndex(latin1);
    qApp->processEvents();

    const QString mojibake = editor.codeEditor()->toPlainText();
    EXPECT_NE(mojibake, chinese);
    // Latin-1 is one byte per character, so the reinterpretation must have the
    // length of the FILE, not of the previously decoded string.
    EXPECT_EQ(mojibake.size(), gbk->fromUnicode(chinese).size());
    // A manual pick needs no extra wording: the combo already shows that codec.
    EXPECT_EQ(editor.encodingStatusText(), QStringLiteral("ISO-8859-1"));

    // Going back to Auto restores the original text exactly. It could not, if
    // the switch had reinterpreted the decoded QString instead of the bytes --
    // that conversion is lossy and does not come back.
    editor.encodingCombo()->setCurrentIndex(0);
    qApp->processEvents();
    EXPECT_EQ(editor.codeEditor()->toPlainText(), chinese);
}

TEST(TextEditorEditingTest, SaveWritesBackInTheEncodingTheBufferWasDecodedWith) {
    // Saving must not silently transcode. A GBK file edited and saved is still
    // a GBK file; turning it into UTF-8 behind the user's back would be a data
    // change nobody asked for.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTextCodec *gbk = QTextCodec::codecForName("GB18030");
    ASSERT_NE(gbk, nullptr);
    const QString chinese = QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87");
    const QString path = writeBytes(dir, QStringLiteral("keep.txt"), gbk->fromUnicode(chinese));

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_EQ(editor.codeEditor()->toPlainText(), chinese);

    typeAtEnd(editor, QStringLiteral("!"));
    ASSERT_TRUE(editor.save());

    const QString expected = chinese + QStringLiteral("!");
    EXPECT_EQ(readBytes(path), gbk->fromUnicode(expected));
    EXPECT_NE(readBytes(path), expected.toUtf8());
}

TEST(TextEditorEditingTest, AManualEncodingChoiceGovernsTheBytesWritten) {
    // The combo is a "read this file as" control, not a "convert to" control:
    // picking a codec re-decodes the SAME bytes, so an unedited round trip is
    // byte-for-byte identical whichever codec is chosen. What the choice does
    // govern is how the buffer is encoded on the way back out, which is what
    // makes correcting a wrong auto-detection actually stick.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QTextCodec *gbk = QTextCodec::codecForName("GB18030");
    ASSERT_NE(gbk, nullptr);
    const QString chinese = QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87");
    const QByteArray gbkBytes = gbk->fromUnicode(chinese);
    const QString path = writeBytes(dir, QStringLiteral("manual.txt"), gbkBytes);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));

    const int latin1 = encodingIndex(editor, QStringLiteral("ISO-8859-1"));
    ASSERT_GE(latin1, 0);
    editor.encodingCombo()->setCurrentIndex(latin1);
    qApp->processEvents();
    ASSERT_TRUE(editor.save());
    // Decoded as Latin-1 and re-encoded as Latin-1: the file is untouched.
    EXPECT_EQ(readBytes(path), gbkBytes);

    // Now an edit is made under that choice, and it is Latin-1 that lands.
    typeAtEnd(editor, QString(QChar(0xE9))); // e-acute: one byte in Latin-1
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(readBytes(path), gbkBytes + QByteArray(1, char(0xE9)));
}

TEST(TextEditorEditingTest, SavePreservesAUtf8ByteOrderMark) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray bom("\xef\xbb\xbf");
    const QString path = writeBytes(dir, QStringLiteral("bom.txt"), bom + "hello\n");

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    // The BOM is consumed, not shown as a stray character at the top of the file.
    EXPECT_EQ(editor.codeEditor()->toPlainText(), QStringLiteral("hello\n"));

    editor.codeEditor()->setPlainText(QStringLiteral("hello there\n"));
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(readBytes(path), bom + "hello there\n");
}

TEST(TextEditorEditingTest, SavePutsBackCrlfLineEndingsQTextDocumentDropped) {
    // QTextDocument cannot store a CR, so without the CRLF bookkeeping every
    // line of a Windows file would come back rewritten as LF -- a whole-file
    // diff produced by opening it and pressing Save.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray crlf("one\r\ntwo\r\n");
    const QString path = writeBytes(dir, QStringLiteral("crlf.txt"), crlf);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    EXPECT_EQ(editor.fileBytes(), crlf);
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(readBytes(path), crlf);

    typeAtEnd(editor, QStringLiteral("three\n"));
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(readBytes(path), QByteArray("one\r\ntwo\r\nthree\r\n"));
}

TEST(TextEditorEditingTest, SaveLeavesAnLfOnlyFileAlone) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray lf("one\ntwo\n");
    const QString path = writeBytes(dir, QStringLiteral("lf.txt"), lf);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    typeAtEnd(editor, QStringLiteral("three\n"));
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(readBytes(path), QByteArray("one\ntwo\nthree\n"));
}

TEST(TextEditorEditingTest, SaveRefreshesTheBytesALaterEncodingChangeReDecodes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeBytes(dir, QStringLiteral("refresh.txt"), "abc\n");

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    editor.codeEditor()->setPlainText(QStringLiteral("abcdef\n"));
    ASSERT_TRUE(editor.save());
    EXPECT_EQ(editor.fileBytes(), QByteArray("abcdef\n"));

    const int latin1 = encodingIndex(editor, QStringLiteral("ISO-8859-1"));
    ASSERT_GE(latin1, 0);
    editor.encodingCombo()->setCurrentIndex(latin1);
    qApp->processEvents();
    // Re-decoded from what was SAVED, not from what was originally opened.
    EXPECT_EQ(editor.codeEditor()->toPlainText(), QStringLiteral("abcdef\n"));
}

TEST(TextEditorEditingTest, ToolbarCarriesSaveAndAnEncodingSelector) {
    TextEditor editor;
    ASSERT_NE(editor.toolBar(), nullptr);
    ASSERT_NE(editor.saveAction(), nullptr);
    ASSERT_NE(editor.encodingCombo(), nullptr);
    EXPECT_TRUE(editor.toolBar()->actions().contains(editor.saveAction()));
    EXPECT_GT(editor.encodingCombo()->count(), 1);
    EXPECT_EQ(editor.encodingCombo()->itemText(0), QStringLiteral("Auto"));
}

TEST(TextEditorEditingTest, SeamAcceptsAnAuxiliaryBarAndAnExtraView) {
    // Guards the two attachment points documented in TextEditor.h for the find
    // bar and the hex view, so a later change cannot quietly remove them.
    TextEditor editor;
    auto *layout = editor.findChild<QVBoxLayout *>();
    ASSERT_NE(layout, nullptr);
    const int before = layout->count();

    auto *findBar = new QWidget;
    editor.addAuxiliaryBar(findBar);
    EXPECT_EQ(layout->count(), before + 1);
    // Between the toolbar and the view stack, not above the toolbar or below
    // the editor.
    EXPECT_GT(layout->indexOf(findBar), layout->indexOf(editor.toolBar()));
    EXPECT_LT(layout->indexOf(findBar), layout->indexOf(editor.viewStack()));

    auto *hexView = new QWidget;
    const int page = editor.addView(hexView);
    EXPECT_EQ(page, 1); // the CodeEditor is page 0
    editor.setCurrentView(page);
    EXPECT_EQ(editor.viewStack()->currentWidget(), hexView);
    editor.setCurrentView(0);
    EXPECT_EQ(editor.viewStack()->currentWidget(), editor.codeEditor());
}
