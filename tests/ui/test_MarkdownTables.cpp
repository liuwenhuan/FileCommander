#include <gtest/gtest.h>

#include <QString>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

#include "QuickView.h"

namespace {

// How many cells in the document's first table actually carry text.
int filledCells(const QString &markdown, int *rows = nullptr, int *columns = nullptr) {
    QTextDocument doc;
    doc.setMarkdown(QuickView::softenTableLineBreaks(markdown),
                    QTextDocument::MarkdownDialectGitHub);
    for (QTextFrame *frame : doc.rootFrame()->childFrames()) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (!table)
            continue;
        if (rows)
            *rows = table->rows();
        if (columns)
            *columns = table->columns();
        int filled = 0;
        for (int r = 0; r < table->rows(); ++r) {
            for (int c = 0; c < table->columns(); ++c) {
                if (!table->cellAt(r, c).firstCursorPosition().block().text().isEmpty())
                    ++filled;
            }
        }
        return filled;
    }
    return -1;
}

} // namespace

// A <br> in a table cell used to empty the rest of the document.
//
// Qt renders Markdown with its bundled MD4C, which reads <br> as the start of
// inline HTML and swallows the rest of the cell -- and with it every remaining
// cell and row. Measured on a real 82 KB file: two <br> tags left 25 of 204
// cells filled, and all 62 tables after that one came out completely blank.
//
// Asserted on what the reader sees -- cells with text in them -- rather than on
// the substitution. A test that checked the string no longer contains "<br>"
// would pass just as well if the replacement broke the table some other way.
TEST(MarkdownTables, ACellWithALineBreakDoesNotEmptyTheRestOfTheTable) {
    const QString table = QStringLiteral(
        "| a | b | c |\n"
        "|---|---|---|\n"
        "| one | two | three |\n"
        "| **four**<br>(note) | five | six |\n"
        "| seven | eight | nine |\n");

    int rows = 0, columns = 0;
    const int filled = filledCells(table, &rows, &columns);
    EXPECT_EQ(rows, 4);
    EXPECT_EQ(columns, 3);
    EXPECT_EQ(filled, 12) << "a <br> in row 2 cost " << (12 - filled) << " cells";
}

// The variants that appear in real documents, and the surrounding text.
TEST(MarkdownTables, EveryFormOfTheTagIsSoftenedAndNothingElseIsTouched) {
    for (const char *tag : {"<br>", "<br/>", "<br />", "<BR>", "<Br />"}) {
        const QString table = QStringLiteral("| a | b |\n|---|---|\n| x%1y | z |\n")
                                  .arg(QString::fromLatin1(tag));
        EXPECT_EQ(filledCells(table), 4) << "not softened: " << tag;
    }

    // Outside a table row the text is left exactly as written: whether <br>
    // misbehaves in prose was never measured, and a previewer should not
    // rewrite text nobody complained about.
    const QString prose = QStringLiteral("a line<br>and another\n");
    EXPECT_EQ(QuickView::softenTableLineBreaks(prose), prose);

    // And a row that has no tag comes back untouched, byte for byte.
    const QString plain = QStringLiteral("| a | b |\n|---|---|\n| x | y |\n");
    EXPECT_EQ(QuickView::softenTableLineBreaks(plain), plain);
}
