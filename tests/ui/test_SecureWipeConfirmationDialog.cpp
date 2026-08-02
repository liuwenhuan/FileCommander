#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTest>

#include "dialogs/SecureWipeConfirmationDialog.h"

namespace {

QPlainTextEdit *pathList(SecureWipeConfirmationDialog &dialog) {
    return dialog.findChild<QPlainTextEdit *>(QStringLiteral("SecureWipePathList"));
}

QLabel *summaryLabel(SecureWipeConfirmationDialog &dialog) {
    return dialog.findChild<QLabel *>(QStringLiteral("SecureWipeSummary"));
}

} // namespace

TEST(SecureWipeConfirmationDialogTest, ShowsCountBytesAndEveryAbsolutePath) {
    const QStringList paths{
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("report.pdf")),
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("archive.zip")),
    };
    SecureWipeConfirmationDialog dialog(paths, 1234567);

    QLabel *summary = summaryLabel(dialog);
    QPlainTextEdit *list = pathList(dialog);
    ASSERT_NE(summary, nullptr);
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("2"))) << summary->text().toStdString();
    EXPECT_TRUE(summary->text().contains(QStringLiteral("1234567")))
        << summary->text().toStdString();
    EXPECT_EQ(list->toPlainText(), paths.join(QLatin1Char('\n')));
}

TEST(SecureWipeConfirmationDialogTest, PathListIsReadOnlySelectableAndCopyable) {
    const QString path =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("private file.txt"));
    SecureWipeConfirmationDialog dialog(QStringList{path}, 42);
    dialog.show();
    QCoreApplication::processEvents();

    QPlainTextEdit *list = pathList(dialog);
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(list->isReadOnly());
    EXPECT_TRUE(list->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    EXPECT_TRUE(list->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));

    list->selectAll();
    list->setFocus();
    QApplication::clipboard()->clear();
    QTest::keyClick(list, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), path);
}

TEST(SecureWipeConfirmationDialogTest, KeepsAboutFourLinesVisibleAndScrollsLongLists) {
    QStringList paths;
    for (int i = 0; i < 8; ++i)
        paths.append(QDir(QDir::tempPath()).absoluteFilePath(
            QStringLiteral("item-%1.txt").arg(i)));

    SecureWipeConfirmationDialog dialog(paths, 800);
    dialog.show();
    QCoreApplication::processEvents();

    QPlainTextEdit *list = pathList(dialog);
    ASSERT_NE(list, nullptr);
    const int lineHeight = list->fontMetrics().lineSpacing();
    EXPECT_GE(list->viewport()->height(), lineHeight * 3);
    EXPECT_LE(list->viewport()->height(), lineHeight * 5);
    EXPECT_GT(list->verticalScrollBar()->maximum(), 0);
}

TEST(SecureWipeConfirmationDialogTest, PathListHeightTracksRuntimeFontChanges) {
    QStringList paths;
    for (int i = 0; i < 8; ++i)
        paths.append(QDir(QDir::tempPath()).absoluteFilePath(
            QStringLiteral("item-%1.txt").arg(i)));

    SecureWipeConfirmationDialog dialog(paths, 800);
    dialog.show();
    QCoreApplication::processEvents();

    QPlainTextEdit *list = pathList(dialog);
    ASSERT_NE(list, nullptr);
    const int initialViewportHeight = list->viewport()->height();

    QFont changed = dialog.font();
    changed.setPointSize(22);
    dialog.setFont(changed);
    QCoreApplication::processEvents();

    const int lineHeight = list->fontMetrics().lineSpacing();
    EXPECT_GT(list->viewport()->height(), initialViewportHeight);
    EXPECT_GE(list->viewport()->height(), lineHeight * 3);
    EXPECT_LE(list->viewport()->height(), lineHeight * 5);
}
