#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>
#include <QTest>

#include <limits>

#include "dialogs/ChecksumDialog.h"

namespace {

QTableWidget *checksumTable(ChecksumDialog &dialog) {
    return dialog.findChild<QTableWidget *>();
}

void seedTable(QTableWidget *table) {
    ASSERT_NE(table, nullptr);
    ASSERT_GE(table->rowCount(), 2);
    for (int row = 0; row < table->rowCount(); ++row) {
        for (int column = 0; column < table->columnCount(); ++column) {
            table->item(row, column)->setText(
                QStringLiteral("r%1c%2").arg(row).arg(column));
        }
    }
}

} // namespace

TEST(ChecksumDialogTest, TableSelectsIndividualItemsAndCopiesOneCellVerbatim) {
    ChecksumDialog dialog(QStringList{QStringLiteral("first"), QStringLiteral("second")});
    dialog.show();
    QCoreApplication::processEvents();

    QTableWidget *table = checksumTable(dialog);
    seedTable(table);
    EXPECT_EQ(table->selectionBehavior(), QAbstractItemView::SelectItems);

    table->clearSelection();
    table->setCurrentCell(0, 2, QItemSelectionModel::ClearAndSelect);
    table->setFocus();
    QApplication::clipboard()->clear();
    QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);

    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("r0c2"));
}

TEST(ChecksumDialogTest, MultipleSelectedCellsCopyInVisualTsvOrder) {
    ChecksumDialog dialog(QStringList{QStringLiteral("first"), QStringLiteral("second")});
    dialog.show();
    QCoreApplication::processEvents();

    QTableWidget *table = checksumTable(dialog);
    seedTable(table);
    table->clearSelection();
    table->setRangeSelected(QTableWidgetSelectionRange(0, 1, 1, 2), true);

    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "copySelection", Qt::DirectConnection));
    EXPECT_EQ(QApplication::clipboard()->text(),
              QStringLiteral("r0c1\tr0c2\nr1c1\tr1c2"));
}

TEST(ChecksumDialogTest, NonContiguousCellsKeepTheirBoundingRectangleCoordinates) {
    ChecksumDialog dialog(
        QStringList{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third")});
    dialog.show();
    QCoreApplication::processEvents();

    QTableWidget *table = checksumTable(dialog);
    seedTable(table);
    table->clearSelection();
    table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 0, 0), true);
    table->setRangeSelected(QTableWidgetSelectionRange(2, 2, 2, 2), true);

    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "copySelection", Qt::DirectConnection));
    EXPECT_EQ(QApplication::clipboard()->text(),
              QStringLiteral("r0c0\t\t\n\t\t\n\t\tr2c2"));
}

TEST(ChecksumDialogTest, ProgressUsesDedicatedPercentLabelInsteadOfBarText) {
    ChecksumDialog dialog(QStringList{QStringLiteral("first")});
    QProgressBar *progress = dialog.findChild<QProgressBar *>();
    ASSERT_NE(progress, nullptr);
    EXPECT_FALSE(progress->isTextVisible());

    QLabel *percent = dialog.findChild<QLabel *>(QStringLiteral("ChecksumProgressPercent"));
    ASSERT_NE(percent, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                                          Q_ARG(qint64, 1), Q_ARG(qint64, 4)));
    EXPECT_EQ(percent->text(), QStringLiteral("25%"));
}

TEST(ChecksumDialogTest, ProgressAvoidsOverflowAndClampsToPercentRange) {
    ChecksumDialog dialog(QStringList{QStringLiteral("first")});
    QProgressBar *progress = dialog.findChild<QProgressBar *>();
    QLabel *percent = dialog.findChild<QLabel *>(QStringLiteral("ChecksumProgressPercent"));
    ASSERT_NE(progress, nullptr);
    ASSERT_NE(percent, nullptr);

    const qint64 maximum = std::numeric_limits<qint64>::max();
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                                          Q_ARG(qint64, maximum / 2 + 1), Q_ARG(qint64, maximum)));
    EXPECT_EQ(progress->value(), 50);
    EXPECT_EQ(percent->text(), QStringLiteral("50%"));

    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                                          Q_ARG(qint64, maximum), Q_ARG(qint64, maximum / 2)));
    EXPECT_EQ(progress->value(), 100);
    EXPECT_EQ(percent->text(), QStringLiteral("100%"));

    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onProgress", Qt::DirectConnection,
                                          Q_ARG(qint64, -1), Q_ARG(qint64, maximum)));
    EXPECT_EQ(progress->value(), 0);
    EXPECT_EQ(percent->text(), QStringLiteral("0%"));
}
