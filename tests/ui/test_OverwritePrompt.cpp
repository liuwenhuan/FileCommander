#include <gtest/gtest.h>

#include "dialogs/OverwriteConfirmDialog.h"

// The text the user reads before deciding to overwrite. It named both files and
// then gave "(0 bytes)" for each of them whenever the transfer was to or from a
// server, because it re-derived the sizes with a QFileInfo over paths that
// belong to the server. It now renders what the operation measured.

TEST(OverwritePromptTest, ShowsTheSizesItWasGiven) {
    FileConflict conflict;
    conflict.sourcePath = QStringLiteral("/share/docs/report.pdf");
    conflict.destPath = QStringLiteral("/home/deepin/report.pdf");
    conflict.sourceSize = 1234567;
    conflict.destSize = 42;

    const QString text = OverwriteConfirmDialog::describe(conflict);
    EXPECT_TRUE(text.contains(QStringLiteral("report.pdf")));
    EXPECT_TRUE(text.contains(conflict.sourcePath));
    EXPECT_TRUE(text.contains(conflict.destPath));
    // Exact bytes for the comparison, readable units for the scale.
    EXPECT_TRUE(text.contains(QStringLiteral("1.2 MB, 1234567 bytes"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("42 B"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("0 bytes"))) << text.toStdString();
}

TEST(OverwritePromptTest, SaysUnknownRatherThanInventingAZero) {
    // A directory, or a backend that could not report a size. "0 bytes" would
    // read as an empty file and invite the user to overwrite it.
    FileConflict conflict;
    conflict.sourcePath = QStringLiteral("/share/docs/folder");
    conflict.destPath = QStringLiteral("/home/deepin/folder");

    const QString text = OverwriteConfirmDialog::describe(conflict);
    EXPECT_FALSE(text.contains(QStringLiteral("0 bytes"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("-1"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("unknown size"))) << text.toStdString();
}

TEST(OverwritePromptTest, NamesTheDestinationFileEvenForARemotePath) {
    // The heading is the destination's file name. Splitting the path is pure
    // string work, so it is right for a server path too -- but nothing else
    // about that path may be looked up on this machine.
    FileConflict conflict;
    conflict.sourcePath = QStringLiteral("/video/第一集.mp4");
    conflict.destPath = QStringLiteral("/backup/第一集.mp4");
    conflict.sourceSize = 0; // a genuinely empty file is still reported as 0 B
    conflict.destSize = 100;

    const QString text = OverwriteConfirmDialog::describe(conflict);
    EXPECT_TRUE(text.startsWith(QStringLiteral("第一集.mp4"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("0 B"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("100 B"))) << text.toStdString();
}
