#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QAbstractItemView>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTemporaryDir>

#include "QuickView.h"
#include "config/Settings.h"

namespace {

QString writeCsv(const QTemporaryDir &dir, const QByteArray &content) {
    const QString path = dir.filePath(QStringLiteral("records.CSV"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    return path;
}

} // namespace

TEST(CsvPreviewTest, RendersQuotedRecordsAsAReadOnlyTable) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeCsv(dir, "name,comment,empty\r\n"
                                       "Alice,\"comma, inside\",\r\n"
                                       "Bob,\"quote \"\"and\"\" newline\r\ninside\",x\r\n");
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    view.resize(640, 400);
    view.show();
    view.showFile(path);
    ASSERT_TRUE(view.waitForTextIdleForTest());

    auto *table = view.findChild<QTableWidget *>(QStringLiteral("quickViewCsvTable"));
    ASSERT_NE(table, nullptr);
    EXPECT_TRUE(table->isVisible());
    EXPECT_EQ(table->rowCount(), 3);
    EXPECT_EQ(table->columnCount(), 3);
    EXPECT_EQ(table->item(1, 1)->text(), QStringLiteral("comma, inside"));
    EXPECT_EQ(table->item(1, 2)->text(), QString());
    EXPECT_EQ(table->item(2, 1)->text(), QStringLiteral("quote \"and\" newline\r\ninside"));
    EXPECT_EQ(table->item(2, 2)->text(), QStringLiteral("x"));
    EXPECT_EQ(table->editTriggers(), QAbstractItemView::NoEditTriggers);
    EXPECT_FALSE(table->horizontalHeader()->isVisible());
    EXPECT_FALSE(table->verticalHeader()->isVisible());

    auto *text = view.findChild<QPlainTextEdit *>(QStringLiteral("quickViewTextView"));
    ASSERT_NE(text, nullptr);
    EXPECT_FALSE(text->isVisible());
}

TEST(CsvPreviewTest, LimitsRowsAndShowsTheExistingTruncationNotice) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray content;
    for (int row = 0; row < 501; ++row)
        content += QByteArray::number(row) + ",value\n";
    const QString path = writeCsv(dir, content);
    ASSERT_FALSE(path.isEmpty());

    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    QuickView view(settings, QuickView::Context::Window);
    view.showFile(path);
    ASSERT_TRUE(view.waitForTextIdleForTest());

    auto *table = view.findChild<QTableWidget *>(QStringLiteral("quickViewCsvTable"));
    auto *notice = view.findChild<QLabel *>(QStringLiteral("quickViewCsvNotice"));
    ASSERT_NE(table, nullptr);
    ASSERT_NE(notice, nullptr);
    EXPECT_EQ(table->rowCount(), 500);
    EXPECT_FALSE(notice->isHidden());
}
