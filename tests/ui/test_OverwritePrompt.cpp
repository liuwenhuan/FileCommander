#include <gtest/gtest.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QTranslator>

#include "dialogs/CompareDialog.h"
#include "dialogs/OverwriteConfirmDialog.h"
#include "dialogs/SyncDialog.h"

namespace {

class LongDialogTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText, const char *, int) const override {
        const QString contextName = QString::fromLatin1(context);
        const QString source = QString::fromUtf8(sourceText);

        if (contextName == QStringLiteral("OverwriteConfirmDialog") &&
            (source == QStringLiteral("Overwrite") || source == QStringLiteral("Overwrite All") ||
             source == QStringLiteral("Skip") || source == QStringLiteral("Skip All") ||
             source == QStringLiteral("Rename"))) {
            return QStringLiteral("Translated action choice: %1").arg(source);
        }

        if (contextName == QStringLiteral("SyncDialog") && source.contains(QStringLiteral("To sync:")))
            return QStringLiteral("Detailed synchronization summary with translated explanatory text: %1")
                .arg(source);

        if (contextName == QStringLiteral("CompareDialog") &&
            source == QStringLiteral("Files are identical")) {
            return QStringLiteral("Detailed translated comparison summary: %1").arg(source);
        }

        return {};
    }
};

class ApplicationUiGuard {
public:
    ApplicationUiGuard() : m_font(QApplication::font()) {
        QFont largeFont = m_font;
        largeFont.setPointSize(20);
        QApplication::setFont(largeFont);
        QApplication::installTranslator(&m_translator);
    }

    ~ApplicationUiGuard() {
        QApplication::removeTranslator(&m_translator);
        QApplication::setFont(m_font);
    }

private:
    QFont m_font;
    LongDialogTranslator m_translator;
};

QString longPath(const QString &side) {
    return QStringLiteral("C:/workspace/%1/").arg(side) +
           QStringLiteral("nested-directory-with-a-long-name/").repeated(12) +
           QStringLiteral("quarterly-report-with-an-extremely-long-file-name.txt");
}

QLabel *labelWithToolTip(QWidget &dialog, const QString &toolTip) {
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->toolTip() == toolTip)
            return label;
    }
    return nullptr;
}

void expectWindowFitsAvailableWidth(QWidget &dialog) {
    dialog.show();
    QApplication::processEvents();

    ASSERT_NE(dialog.screen(), nullptr);
    const int availableWidth = dialog.screen()->availableGeometry().width();
    EXPECT_LE(dialog.minimumSizeHint().width(), availableWidth);
    EXPECT_LE(dialog.frameGeometry().width(), availableWidth);
}

} // namespace

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

TEST(OverwritePromptTest, LargeTranslatedButtonsReflowAndRemainVisible) {
    ApplicationUiGuard ui;
    FileConflict conflict;
    conflict.sourcePath = longPath(QStringLiteral("source"));
    conflict.destPath = longPath(QStringLiteral("destination"));

    OverwriteConfirmDialog dialog(conflict);
    expectWindowFitsAvailableWidth(dialog);

    auto *buttonBox = dialog.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttonBox, nullptr);
    const QList<QPushButton *> buttons = buttonBox->findChildren<QPushButton *>();
    ASSERT_EQ(buttons.size(), 6);
    const QRect availableGeometry = dialog.screen()->availableGeometry();

    QSet<int> buttonRows;
    for (QPushButton *button : buttons) {
        EXPECT_TRUE(button->isVisible());
        const QRect inDialog(button->mapTo(&dialog, QPoint(0, 0)), button->size());
        EXPECT_TRUE(dialog.rect().contains(inDialog)) << button->text().toStdString();
        const QRect onScreen(button->mapToGlobal(QPoint(0, 0)), button->size());
        EXPECT_TRUE(availableGeometry.contains(onScreen)) << button->text().toStdString();
        buttonRows.insert(button->mapTo(&dialog, QPoint(0, 0)).y());
    }
    EXPECT_GT(buttonRows.size(), 1);
}

TEST(OverwritePromptTest, SyncDialogConstrainsLongPathsAndSummary) {
    ApplicationUiGuard ui;
    const QString left = longPath(QStringLiteral("left"));
    const QString right = longPath(QStringLiteral("right"));

    SyncDialog dialog(left, {}, right, {});
    expectWindowFitsAvailableWidth(dialog);

    QLabel *leftLabel = labelWithToolTip(dialog, left);
    QLabel *rightLabel = labelWithToolTip(dialog, right);
    ASSERT_NE(leftLabel, nullptr);
    ASSERT_NE(rightLabel, nullptr);
    EXPECT_LE(leftLabel->width() + rightLabel->width(), dialog.contentsRect().width());

    bool foundWrappingSummary = false;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("Detailed synchronization summary"))) {
            foundWrappingSummary = label->wordWrap();
            break;
        }
    }
    EXPECT_TRUE(foundWrappingSummary);
}

TEST(OverwritePromptTest, CompareDialogConstrainsLongPathsAndSummary) {
    ApplicationUiGuard ui;
    const QString left = longPath(QStringLiteral("left"));
    const QString right = longPath(QStringLiteral("right"));

    CompareDialog dialog(QString(), QString(), left, right);
    expectWindowFitsAvailableWidth(dialog);

    QLabel *leftLabel = labelWithToolTip(dialog, left);
    QLabel *rightLabel = labelWithToolTip(dialog, right);
    ASSERT_NE(leftLabel, nullptr);
    ASSERT_NE(rightLabel, nullptr);
    EXPECT_LE(leftLabel->width() + rightLabel->width(), dialog.contentsRect().width());

    bool foundWrappingSummary = false;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("Detailed translated comparison summary"))) {
            foundWrappingSummary = label->wordWrap();
            break;
        }
    }
    EXPECT_TRUE(foundWrappingSummary);
}
