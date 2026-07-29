#include <gtest/gtest.h>

#include <QDialogButtonBox>
#include <QFile>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QPushButton>
#include <QTableWidget>

#include "dialogs/ShortcutsDialog.h"

namespace {

const QList<QPair<QString, QString>> kActions = {
    {QStringLiteral("copy"), QStringLiteral("Copy")},
    {QStringLiteral("move"), QStringLiteral("Move")},
};

const QMap<QString, QKeySequence> kShortcuts = {
    {QStringLiteral("copy"), QKeySequence(QStringLiteral("Ctrl+C"))},
    {QStringLiteral("move"), QKeySequence(QStringLiteral("F6"))},
};

} // namespace

TEST(ShortcutsDialogTest, ShortcutColumnIsInteractiveAndKeepsUsefulMinimumWidth) {
    ShortcutsDialog dialog(kActions, kShortcuts, kShortcuts);
    QTableWidget *table = dialog.findChild<QTableWidget *>(QStringLiteral("ShortcutTable"));
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->objectName(), QStringLiteral("ShortcutTable"));

    QHeaderView *header = table->horizontalHeader();
    EXPECT_EQ(header->sectionResizeMode(1), QHeaderView::Interactive);
    EXPECT_GE(table->columnWidth(1), 200);
    EXPECT_GE(dialog.minimumWidth(), 480);

    const auto editors = table->findChildren<QKeySequenceEdit *>();
    ASSERT_FALSE(editors.isEmpty());
    EXPECT_GE(editors.first()->minimumWidth(), 200);
    for (QKeySequenceEdit *editor : editors)
        EXPECT_EQ(editor->objectName(), QStringLiteral("ShortcutSequenceEdit"));
}

TEST(ShortcutsDialogTest, ThemeSheetsKeepShortcutEditorsTransparentAndBorderless) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());
        EXPECT_TRUE(sheet.contains(QStringLiteral(
            "QTableWidget#ShortcutTable QKeySequenceEdit")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QStringLiteral("border: none")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QStringLiteral("background: transparent")))
            << theme.toStdString();
    }
}

TEST(ShortcutsDialogTest, ConflictDetectionStillDisablesAcceptance) {
    ShortcutsDialog dialog(kActions, kShortcuts, kShortcuts);
    QTableWidget *table = dialog.findChild<QTableWidget *>(QStringLiteral("ShortcutTable"));
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->objectName(), QStringLiteral("ShortcutTable"));
    auto *secondEditor = qobject_cast<QKeySequenceEdit *>(table->cellWidget(1, 1));
    ASSERT_NE(secondEditor, nullptr);

    secondEditor->setKeySequence(QKeySequence(QStringLiteral("Ctrl+C")));
    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "onSequenceEdited", Qt::DirectConnection,
                                          Q_ARG(int, 1)));

    QDialogButtonBox *buttons = dialog.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttons, nullptr);
    EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
}
