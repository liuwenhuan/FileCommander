#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>

#include "ByteSearch.h"
#include "FindBar.h"
#include "HexEditor.h"
#include "TextEditor.h"
#include "TryUntil.h"

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

QString writeSparseBinary(const QTemporaryDir &dir, const QString &name, qint64 size,
                          const QByteArray &head, const QByteArray &tail) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.resize(size) ||
        file.write(head) != head.size() || !file.seek(size - tail.size()) ||
        file.write(tail) != tail.size())
        return {};
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

TEST(TextEditorIntegration, AHugeBinaryStartsWithALazyHexPrefixAndPreservesItsTailOnSave) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 originalSize = HexEditor::maximumSize() + 1024 * 1024;
    const QByteArray head("\x7f\x45\x4c\x46\x02\x01\x01\x00", 8);
    QByteArray tail(4096, '\0');
    for (int i = 0; i < tail.size(); ++i)
        tail[i] = char(i & 0xff);
    tail.replace(100, 16, QByteArray("HUGE-BINARY-TAIL", 16));
    const QString path = writeSparseBinary(dir, QStringLiteral("huge.bin"), originalSize,
                                           head, tail);
    ASSERT_FALSE(path.isEmpty());

    // A regression must not leave a blocking over-size dialog behind. The
    // timer makes a red run finish while still proving no modal was shown.
    int modals = 0;
    QTimer dismissModal;
    dismissModal.setInterval(10);
    QObject::connect(&dismissModal, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        ++modals;
        QMetaObject::invokeMethod(modal, "reject", Qt::QueuedConnection);
    });
    dismissModal.start();

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    dismissModal.stop();
    qApp->processEvents();

    EXPECT_EQ(modals, 0);
    ASSERT_TRUE(editor.isHexMode());
    ASSERT_NE(editor.hexEditor(), nullptr);
    EXPECT_LT(editor.hexEditor()->contents().size(), QFileInfo(path).size());
    auto *partial = editor.toolBar()->findChild<QLabel *>(QStringLiteral("textEditorPartial"));
    ASSERT_NE(partial, nullptr);
    EXPECT_FALSE(partial->text().isEmpty());
    QAction *loadRemainder = nullptr;
    for (QAction *action : editor.toolBar()->actions()) {
        if (action->text() == QStringLiteral("Load remainder")) {
            loadRemainder = action;
            break;
        }
    }
    ASSERT_NE(loadRemainder, nullptr);
    EXPECT_FALSE(loadRemainder->isEnabled());

    const QByteArray loadedPrefix = editor.hexEditor()->contents();
    const QByteArray inserted("\xaa\xbb\xcc", 3);
    QByteArray editedPrefix = loadedPrefix;
    editedPrefix.insert(16, inserted);
    ASSERT_TRUE(editor.hexEditor()->setContents(editedPrefix));
    editor.hexEditor()->setModified(true);
    ASSERT_TRUE(editor.save());

    QFile saved(path);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.size(), originalSize + inserted.size());
    ASSERT_TRUE(saved.seek(originalSize - tail.size() + inserted.size()));
    EXPECT_EQ(saved.read(tail.size()), tail);
    ASSERT_TRUE(saved.seek(0));
    EXPECT_EQ(saved.read(head.size()), head);
}

TEST(TextEditorIntegration, AHugeBinaryAutosaveAndCloseFlushKeepItsUnloadedTail) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const qint64 originalSize = HexEditor::maximumSize() + 1024 * 1024;
    const QByteArray head("\x7f\x45\x4c\x46\x02\x01\x01\x00", 8);
    const QByteArray tail("HUGE-BINARY-TAIL", 16);
    const QString path = writeSparseBinary(dir, QStringLiteral("huge-flush.bin"), originalSize,
                                           head, tail);
    ASSERT_FALSE(path.isEmpty());

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_TRUE(editor.isHexMode());

    QByteArray autosaved = editor.hexEditor()->contents();
    autosaved[8] = char(0xfe);
    ASSERT_TRUE(editor.hexEditor()->setContents(autosaved));
    editor.hexEditor()->setModified(true);
    FC_TRY_VERIFY_WITH_TIMEOUT(([&]() {
        QFile saved(path);
        return saved.open(QIODevice::ReadOnly) && saved.size() == originalSize &&
               saved.seek(originalSize - tail.size()) && saved.read(tail.size()) == tail;
    })(), 5000);

    QByteArray closeFlushed = editor.hexEditor()->contents();
    closeFlushed[9] = char(0xfd);
    ASSERT_TRUE(editor.hexEditor()->setContents(closeFlushed));
    editor.hexEditor()->setModified(true);
    ASSERT_TRUE(editor.promptSaveIfModified());

    QFile saved(path);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.size(), originalSize);
    ASSERT_TRUE(saved.seek(originalSize - tail.size()));
    EXPECT_EQ(saved.read(tail.size()), tail);
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

    // Still untouched before the debounce expires; explicit Save remains an
    // immediate flush and is what this test exercises.
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

TEST(TextEditorIntegration, HexEditsAreAutomaticallySavedAfterTheDebounce) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray binary("\x00\x01\x02\x03", 4);
    binary.append(QByteArray(300, '\0'));
    const QString path = writeFile(dir, QStringLiteral("autosave.bin"), binary);

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_TRUE(editor.isHexMode());
    QByteArray edited = binary;
    edited[2] = char(0xFE);
    editor.hexEditor()->setContents(edited);
    editor.hexEditor()->setModified(true);

    QFile before(path);
    ASSERT_TRUE(before.open(QIODevice::ReadOnly));
    EXPECT_EQ(before.readAll(), binary);
    before.close();

    FC_TRY_VERIFY_WITH_TIMEOUT(([&]() {
        QFile saved(path);
        return saved.open(QIODevice::ReadOnly) && saved.readAll() == edited;
    })(), 3000);
    EXPECT_FALSE(editor.isDocumentModified());
}

TEST(TextEditorIntegration, FailedAutosaveWarnsOnceAndKeepsTheBufferDirty) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        GTEST_SKIP() << "the Windows offscreen plugin crashes while showing QMessageBox";
#endif
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, QStringLiteral("blocked.txt"), QByteArray("original\n"));

    TextEditor editor;
    ASSERT_TRUE(editor.loadFile(path));
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_TRUE(QDir().mkdir(path)); // QFile cannot truncate a directory

    int warnings = 0;
    QTimer dismissWarnings;
    dismissWarnings.setInterval(10);
    QObject::connect(&dismissWarnings, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal || modal->property("autosave-test-counted").toBool())
            return;
        modal->setProperty("autosave-test-counted", true);
        ++warnings;
        QMetaObject::invokeMethod(modal, "reject", Qt::QueuedConnection);
    });
    dismissWarnings.start();

    QTextCursor cursor(editor.codeEditor()->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("still in memory\n"));

    FC_TRY_COMPARE_WITH_TIMEOUT(warnings, 1, 3000);
    QTest::qWait(900); // a blocked autosave must not retry in the background
    qApp->processEvents();
    EXPECT_EQ(warnings, 1);
    EXPECT_TRUE(editor.isDocumentModified());
    EXPECT_TRUE(editor.codeEditor()->toPlainText().endsWith(QStringLiteral("still in memory\n")));
    EXPECT_FALSE(editor.promptSaveIfModified()); // vetoes Preview/close, no second dialog
    EXPECT_EQ(warnings, 1);

    dismissWarnings.stop();
    ASSERT_TRUE(QDir(path).removeRecursively());
    ASSERT_TRUE(editor.save()); // explicit Save is always allowed to retry
    EXPECT_FALSE(editor.isDocumentModified());
}
