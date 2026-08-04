#include <gtest/gtest.h>

#include "HexEditor.h"

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPixmap>
#include <QUndoStack>
#include <QVector>

#include <iostream>
#include <memory>

namespace {

// The widget needs a QApplication; the suite is shared with tests that do not.
// Creating it on first use keeps that cost off them and keeps this file free of
// a custom main().
void ensureApplication() {
    if (qApp)
        return;
#ifndef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    static int argc = 1;
    static char name[] = "viewer_tests";
    static char *argv[] = {name, nullptr};
    static QApplication application(argc, argv);
    Q_UNUSED(application);
}

class HexEditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApplication();
        editor = new HexEditor;
        editor->resize(900, 600);
        ASSERT_TRUE(editor->setContents(QByteArrayLiteral("ABCDEFGH")));
    }

    void TearDown() override { delete editor; }

    void type(int key, const QString &text,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(editor, &press);
    }

    void typeHex(const QString &digits) {
        for (const QChar digit : digits)
            type(Qt::Key_0 + digit.digitValue(), QString(digit));
    }

    HexEditor *editor = nullptr;
};

} // namespace

TEST_F(HexEditorTest, LoadedContentsComeBackUnchanged) {
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABCDEFGH"));
    EXPECT_EQ(editor->size(), 8);
    EXPECT_FALSE(editor->isModified());
    EXPECT_TRUE(editor->isOverwriteMode());
}

TEST_F(HexEditorTest, TypingInTheHexColumnChangesContentsAndMarksModified) {
    // Shared rather than captured by reference: the connection outlives this
    // scope, and a test must not be the thing that crashes on teardown.
    auto modified = std::make_shared<QVector<bool>>();
    QObject::connect(editor, &HexEditor::modificationChanged, editor,
                     [modified](bool value) { modified->append(value); });

    editor->setCursorPosition(1);
    typeHex(QStringLiteral("7"));
    // The high nibble alone is already an edit; 'B' (0x42) becomes 0x72.
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ArCDEFGH"));

    type(Qt::Key_F, QStringLiteral("f"));
    EXPECT_EQ(static_cast<uchar>(editor->contents().at(1)), 0x7fu);
    EXPECT_TRUE(editor->isModified());
    // Both nibbles are one edit, so the flag flips once, not twice.
    ASSERT_EQ(modified->size(), 1);
    EXPECT_TRUE(modified->at(0));
}

TEST_F(HexEditorTest, TypingInTheAsciiColumnChangesContents) {
    editor->setActiveColumn(HexEditor::Column::Ascii);
    editor->setCursorPosition(2);
    type(Qt::Key_Z, QStringLiteral("Z"));

    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABZDEFGH"));
    EXPECT_EQ(editor->cursorPosition(), 3);
    EXPECT_TRUE(editor->isModified());
}

// Both nibbles of one byte are one edit as far as the user is concerned, so
// they have to be one undo step too.
TEST_F(HexEditorTest, UndoRestoresTheOriginalByteInOneStep) {
    editor->setCursorPosition(0);
    typeHex(QStringLiteral("3"));
    type(Qt::Key_D, QStringLiteral("d"));
    ASSERT_EQ(static_cast<uchar>(editor->contents().at(0)), 0x3du);
    ASSERT_EQ(editor->undoStack()->count(), 1);

    editor->undo();
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABCDEFGH"));
    EXPECT_FALSE(editor->isModified());

    editor->redo();
    EXPECT_EQ(static_cast<uchar>(editor->contents().at(0)), 0x3du);
    EXPECT_TRUE(editor->isModified());
}

TEST_F(HexEditorTest, SeparateBytesAreSeparateUndoSteps) {
    editor->setActiveColumn(HexEditor::Column::Ascii);
    editor->setCursorPosition(0);
    type(Qt::Key_X, QStringLiteral("X"));
    type(Qt::Key_Y, QStringLiteral("Y"));
    ASSERT_EQ(editor->contents(), QByteArrayLiteral("XYCDEFGH"));

    editor->undo();
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("XBCDEFGH"));
    editor->undo();
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABCDEFGH"));
    EXPECT_FALSE(editor->isModified());
}

TEST_F(HexEditorTest, OverwriteModeNeverChangesTheFileSize) {
    editor->setCursorPosition(0);
    typeHex(QStringLiteral("1"));
    type(Qt::Key_A, QStringLiteral("a"));
    typeHex(QStringLiteral("2"));
    type(Qt::Key_B, QStringLiteral("b"));

    EXPECT_EQ(editor->size(), 8);
    EXPECT_EQ(static_cast<uchar>(editor->contents().at(0)), 0x1au);
    EXPECT_EQ(static_cast<uchar>(editor->contents().at(1)), 0x2bu);
}

TEST_F(HexEditorTest, InsertModeGrowsTheFileAndUndoTakesTheByteBackOut) {
    editor->setOverwriteMode(false);
    editor->setCursorPosition(0);
    typeHex(QStringLiteral("9"));
    type(Qt::Key_E, QStringLiteral("e"));

    ASSERT_EQ(editor->size(), 9);
    EXPECT_EQ(static_cast<uchar>(editor->contents().at(0)), 0x9eu);
    EXPECT_EQ(editor->contents().mid(1), QByteArrayLiteral("ABCDEFGH"));

    editor->undo();
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABCDEFGH"));
    EXPECT_FALSE(editor->isModified());
}

TEST_F(HexEditorTest, DeleteZeroFillsASelectionInOverwriteModeAndRemovesItInInsertMode) {
    editor->selectRange(2, 3);
    type(Qt::Key_Delete, QString());
    EXPECT_EQ(editor->size(), 8);
    EXPECT_EQ(editor->contents(), QByteArray("AB\0\0\0FGH", 8));

    editor->undo();
    ASSERT_EQ(editor->contents(), QByteArrayLiteral("ABCDEFGH"));

    editor->setOverwriteMode(false);
    editor->selectRange(2, 3);
    type(Qt::Key_Delete, QString());
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABFGH"));
}

TEST_F(HexEditorTest, SavingRebaselinesTheModifiedFlagWithoutTouchingContents) {
    editor->setActiveColumn(HexEditor::Column::Ascii);
    editor->setCursorPosition(0);
    type(Qt::Key_Q, QStringLiteral("Q"));
    ASSERT_TRUE(editor->isModified());

    // What the toolbar squad calls after writing contents() to disk.
    editor->setModified(false);
    EXPECT_FALSE(editor->isModified());
    EXPECT_EQ(editor->contents(), QByteArrayLiteral("QBCDEFGH"));

    type(Qt::Key_R, QStringLiteral("R"));
    EXPECT_TRUE(editor->isModified());
}

TEST_F(HexEditorTest, ReadOnlyRefusesEdits) {
    editor->setReadOnly(true);
    editor->setCursorPosition(0);
    typeHex(QStringLiteral("11"));
    type(Qt::Key_Delete, QString());

    EXPECT_EQ(editor->contents(), QByteArrayLiteral("ABCDEFGH"));
    EXPECT_FALSE(editor->isModified());
}

TEST_F(HexEditorTest, SelectionExposesTheSelectedBytes) {
    editor->selectRange(3, 4);
    EXPECT_EQ(editor->selectionStart(), 3);
    EXPECT_EQ(editor->selectionLength(), 4);
    EXPECT_EQ(editor->selectedBytes(), QByteArrayLiteral("DEFG"));
}

TEST_F(HexEditorTest, TheEditorNeverWritesToDisk) {
    // contents() is the only way bytes leave this widget: it has no path, no
    // QFile, and no save entry point, which is what keeps an unsaved edit off
    // the disk. Pinned as a signal to anyone tempted to add one here.
    EXPECT_EQ(editor->metaObject()->indexOfMethod("save()"), -1);
    EXPECT_EQ(editor->metaObject()->indexOfMethod("saveAs()"), -1);
}

// --- size limit ------------------------------------------------------------

TEST(HexEditorLimitTest, TheLimitIsStatedBeforeAnythingIsRead) {
    EXPECT_EQ(HexEditor::maximumSize(), 256LL * 1024 * 1024);
    EXPECT_TRUE(HexEditor::fitsInEditor(0));
    EXPECT_TRUE(HexEditor::fitsInEditor(HexEditor::maximumSize()));
    EXPECT_FALSE(HexEditor::fitsInEditor(HexEditor::maximumSize() + 1));
    EXPECT_FALSE(HexEditor::fitsInEditor(-1));
}

TEST(HexEditorLimitTest, OversizeMessageNamesBothSizes) {
    ensureApplication();
    const QString message = HexEditor::oversizeMessage(3LL * 1024 * 1024 * 1024);
    EXPECT_FALSE(message.isEmpty());
    EXPECT_TRUE(message.contains(QStringLiteral("3")));
    EXPECT_TRUE(message.contains(QStringLiteral("256")));
}

TEST(HexEditorLimitTest, OversizeDataIsRefusedAndLeavesTheEditorAlone) {
    ensureApplication();
    HexEditor editor;
    ASSERT_TRUE(editor.setContents(QByteArrayLiteral("keep me")));

    const QByteArray tooBig(static_cast<int>(HexEditor::maximumSize()) + 1, 'x');
    EXPECT_FALSE(editor.setContents(tooBig));
    EXPECT_EQ(editor.contents(), QByteArrayLiteral("keep me"));
    EXPECT_FALSE(editor.isModified());
}

// --- large files -----------------------------------------------------------

// Layout and painting are per visible row, so neither may scale with the file.
// A 64 MiB buffer is large enough to catch an accidental whole-file walk and
// small enough to stay a unit test; the ceiling itself is measured by
// DISABLED_MeasuresTheDocumentedCeiling below.
TEST(HexEditorLargeFileTest, ScrollingAndPaintingDoNotScaleWithFileSize) {
    ensureApplication();
    HexEditor editor;
    editor.resize(900, 600);
    QByteArray data(64 * 1024 * 1024, '\0');
    for (int i = 0; i < data.size(); i += 997)
        data[i] = static_cast<char>(i);
    ASSERT_TRUE(editor.setContents(data));

    // Realising the widget and warming its font metrics costs a few hundred
    // milliseconds once, whatever the file size. Paying it before the timer
    // starts is the difference between measuring the thing under test and
    // measuring the graphics stack waking up.
    QPixmap canvas(900, 600);
    editor.render(&canvas);

    QElapsedTimer timer;
    timer.start();
    editor.setCursorPosition(editor.size() - 1);
    editor.render(&canvas);
    const qint64 elapsed = timer.elapsed();

    EXPECT_EQ(editor.contents().size(), data.size());
    // Measured at ~10 ms in a Debug build. The regression this guards against
    // is anything that walks the whole buffer per repaint or per cursor move,
    // which at 64 MiB costs orders of magnitude more than this budget.
    EXPECT_LT(elapsed, 300) << "seek to end plus one repaint took " << elapsed << " ms";
}

// Produces the numbers quoted for maximumSize(). Run it by hand:
//   viewer_tests --gtest_also_run_disabled_tests \
//                --gtest_filter=*MeasuresTheDocumentedCeiling*
// It is not part of the normal run because it allocates half a gigabyte and
// reports timings rather than asserting on them.
TEST(HexEditorLargeFileTest, DISABLED_MeasuresTheDocumentedCeiling) {
    ensureApplication();
    QPixmap canvas(900, 600);
    QKeyEvent digit(QEvent::KeyPress, Qt::Key_4, Qt::NoModifier, QStringLiteral("4"));
    QElapsedTimer timer;

    // Baseline: whatever the first render costs on a trivial file is widget
    // realisation, not file size, and has to be subtracted by eye below.
    {
        HexEditor editor;
        editor.resize(900, 600);
        timer.start();
        editor.setContents(QByteArray(1024, '\x5a'));
        std::cout << "[measure] 1 KiB setContents: " << timer.restart() << " ms" << std::endl;
        editor.render(&canvas);
        std::cout << "[measure] 1 KiB first paint: " << timer.restart() << " ms" << std::endl;
        editor.render(&canvas);
        std::cout << "[measure] 1 KiB second paint: " << timer.restart() << " ms" << std::endl;
    }

    HexEditor editor;
    editor.resize(900, 600);
    timer.restart();
    ASSERT_TRUE(editor.setContents(QByteArray(static_cast<int>(HexEditor::maximumSize()), '\x5a')));
    std::cout << "[measure] ceiling setContents: " << timer.restart() << " ms" << std::endl;

    editor.render(&canvas);
    std::cout << "[measure] ceiling first paint: " << timer.restart() << " ms" << std::endl;
    editor.render(&canvas);
    std::cout << "[measure] ceiling second paint: " << timer.restart() << " ms" << std::endl;

    editor.setCursorPosition(editor.size() / 2);
    editor.render(&canvas);
    std::cout << "[measure] ceiling seek to middle plus paint: " << timer.restart() << " ms"
              << std::endl;

    editor.setCursorPosition(0);
    QApplication::sendEvent(&editor, &digit);
    std::cout << "[measure] ceiling first overwrite keystroke (detaches the buffer): "
              << timer.restart() << " ms" << std::endl;
    QApplication::sendEvent(&editor, &digit);
    std::cout << "[measure] ceiling later overwrite keystroke: " << timer.restart() << " ms"
              << std::endl;

    // At exactly the ceiling the file may not grow, so insert mode is measured
    // just under it -- this is the memmove that sets the ceiling in the first
    // place, and offset 0 is its worst case.
    editor.setContents(QByteArray(static_cast<int>(HexEditor::maximumSize()) - 1024, '\x5a'));
    editor.setOverwriteMode(false);
    editor.setCursorPosition(0);
    timer.restart();
    QApplication::sendEvent(&editor, &digit);
    std::cout << "[measure] near-ceiling insert keystroke at offset 0: " << timer.restart()
              << " ms" << std::endl;
    editor.undo();
    std::cout << "[measure] undo of that insert: " << timer.restart() << " ms" << std::endl;

    // And at the ceiling itself growth is refused rather than attempted.
    editor.setContents(QByteArray(static_cast<int>(HexEditor::maximumSize()), '\x5a'));
    editor.setOverwriteMode(false);
    editor.setCursorPosition(0);
    QApplication::sendEvent(&editor, &digit);
    EXPECT_EQ(editor.size(), HexEditor::maximumSize());
}
