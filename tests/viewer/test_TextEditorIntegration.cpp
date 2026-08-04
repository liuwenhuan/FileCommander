#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QStackedWidget>
#include <QTemporaryDir>

#include "ByteSearch.h"
#include "FindBar.h"
#include "HexEditor.h"
#include "TextEditor.h"

namespace {

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(bytes);
    file.close();
    return path;
}

} // namespace

// The three pieces were built separately; this is the seam between them. A file
// that is not text must not be decoded into a text buffer -- saving it would
// write back the decoder's guesses rather than the bytes that were read.
TEST(TextEditorIntegration, ABinaryFileOpensInTheHexView) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray binary("\x7f\x45\x4c\x46\x02\x01\x01", 7);
    binary.append(QByteArray(200, '\0'));
    binary.append("payload");
    const QString path = writeFile(dir, QStringLiteral("a.bin"), binary);
    ASSERT_FALSE(path.isEmpty());

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    EXPECT_TRUE(editor.isHexMode());
    ASSERT_NE(editor.hexEditor(), nullptr);
    EXPECT_EQ(editor.hexEditor()->contents(), binary);
    EXPECT_EQ(editor.viewStack()->currentWidget(), editor.hexEditor());
}

TEST(TextEditorIntegration, ATextFileStaysInTheTextView) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("a.txt"),
                                   QByteArray("hello\nworld\n"));
    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    EXPECT_FALSE(editor.isHexMode());
    EXPECT_EQ(editor.viewStack()->currentIndex(), 0);
}

// The bug this integration would otherwise have shipped: the modified state,
// the title star, the Save button and the close prompt all read the TEXT
// document. In hex mode that never changes, so an edited binary file would
// have shown no star, kept Save greyed out, and been closed without a prompt.
TEST(TextEditorIntegration, EditingInHexModeIsVisibleAsAModification) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray binary("\x00\x01\x02\x03", 4);
    binary.append(QByteArray(300, '\0'));
    const QString path = writeFile(dir, QStringLiteral("b.bin"), binary);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_TRUE(editor.isHexMode());
    EXPECT_FALSE(editor.isDocumentModified());
    EXPECT_FALSE(editor.windowTitle().endsWith(QStringLiteral("*")));

    QByteArray edited = binary;
    edited[0] = char(0xff);
    editor.hexEditor()->setContents(edited);
    editor.hexEditor()->setModified(true);
    qApp->processEvents();

    EXPECT_TRUE(editor.isDocumentModified()) << "a hex edit is invisible to the window";
    EXPECT_TRUE(editor.windowTitle().endsWith(QStringLiteral("*")));
}

// Save has to take the bytes from whichever view owns the document.
TEST(TextEditorIntegration, SavingInHexModeWritesTheEditedBytes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray binary("\x00\x01\x02\x03", 4);
    binary.append(QByteArray(300, '\0'));
    const QString path = writeFile(dir, QStringLiteral("c.bin"), binary);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_TRUE(editor.isHexMode());
    QByteArray edited = binary;
    edited[1] = char(0xAB);
    editor.hexEditor()->setContents(edited);
    editor.hexEditor()->setModified(true);

    // Still untouched on disk: nothing is written until Save.
    QFile before(path);
    ASSERT_TRUE(before.open(QIODevice::ReadOnly));
    EXPECT_EQ(before.readAll(), binary) << "the edit reached disk without a save";
    before.close();

    ASSERT_TRUE(editor.save());
    QFile after(path);
    ASSERT_TRUE(after.open(QIODevice::ReadOnly));
    EXPECT_EQ(after.readAll(), edited);
    after.close();
    EXPECT_FALSE(editor.isDocumentModified()) << "still modified after a successful save";
}

// One find bar serves both views because it searches the file's BYTES.
TEST(TextEditorIntegration, TheFindBarFindsBytesInTheHexView) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray binary(120, '\0');
    binary.replace(64, 2, QByteArray("\x4d\x5a", 2));
    const QString path = writeFile(dir, QStringLiteral("d.bin"), binary);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_TRUE(editor.isHexMode());
    editor.showFindBar();
    ASSERT_NE(editor.findBar(), nullptr);

    const ByteSearch::Needle needle = ByteSearch::parseHex(QStringLiteral("4D 5A"));
    ASSERT_TRUE(needle.valid) << needle.error.toStdString();
    editor.runSearch(needle, ByteSearch::Direction::Forward);
    EXPECT_EQ(editor.hexEditor()->selectionStart(), 64);
    EXPECT_EQ(editor.hexEditor()->selectionLength(), 2);
}

// ...and in the text view the byte offset has to be converted to a character
// position, or a multi-byte encoding puts the caret in the wrong place.
TEST(TextEditorIntegration, TheFindBarLandsOnTheRightCharacterInAMultiByteFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Four Chinese characters (3 bytes each in UTF-8), then a marker. The
    // marker's BYTE offset is 12 but its CHARACTER offset is 4.
    const QByteArray text = QString::fromUtf8("\u4f60\u597d\u4e16\u754cMARK\n").toUtf8();
    const QString path = writeFile(dir, QStringLiteral("e.txt"), text);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_FALSE(editor.isHexMode());
    editor.showFindBar();

    const ByteSearch::Needle needle =
        ByteSearch::compile(QStringLiteral("MARK"), ByteSearch::Mode::Text, "UTF-8",
                            ByteSearch::CaseFolding::Exact);
    ASSERT_TRUE(needle.valid) << needle.error.toStdString();
    editor.runSearch(needle, ByteSearch::Direction::Forward);

    const QTextCursor cursor = editor.codeEditor()->textCursor();
    EXPECT_EQ(cursor.selectedText(), QStringLiteral("MARK"));
    EXPECT_EQ(cursor.selectionStart(), 4)
        << "the byte offset was used as a character position";
}
