#include <gtest/gtest.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFontDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include "ThemedDialogs.h"
#include "TranslationManager.h"

namespace {

QString messageButtonText(const QMessageBox &box, QMessageBox::StandardButton button) {
    const QAbstractButton *abstractButton = box.button(button);
    return abstractButton ? abstractButton->text() : QString();
}

QString dialogButtonText(const QDialogButtonBox &box, QDialogButtonBox::StandardButton button) {
    const QAbstractButton *abstractButton = box.button(button);
    return abstractButton ? abstractButton->text() : QString();
}

void switchLanguage(const QString &language) {
    TranslationManager::switchTo(*qApp, language);
    qApp->processEvents();
}

} // namespace

TEST(StandardButtonLocalizationTest, LocalizesMessageAndDialogButtonBoxesWithQtBaseCatalog) {
    switchLanguage(QStringLiteral("zh_CN"));

    QMessageBox message;
    message.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Ok |
                               QMessageBox::Cancel | QMessageBox::Close | QMessageBox::Save |
                               QMessageBox::Discard);
    ttc::localizeStandardButtons(&message);

    EXPECT_EQ(messageButtonText(message, QMessageBox::Yes), QStringLiteral("是(&Y)"));
    EXPECT_EQ(messageButtonText(message, QMessageBox::No), QStringLiteral("否(&N)"));
    EXPECT_EQ(messageButtonText(message, QMessageBox::Ok), QStringLiteral("确定"));
    EXPECT_EQ(messageButtonText(message, QMessageBox::Cancel), QStringLiteral("取消"));
    EXPECT_EQ(messageButtonText(message, QMessageBox::Close), QStringLiteral("关闭"));
    EXPECT_EQ(messageButtonText(message, QMessageBox::Save), QStringLiteral("保存"));
    EXPECT_EQ(messageButtonText(message, QMessageBox::Discard), QStringLiteral("丢弃"));

    QDialogButtonBox buttons(QDialogButtonBox::Yes | QDialogButtonBox::No | QDialogButtonBox::Ok |
                             QDialogButtonBox::Cancel | QDialogButtonBox::Close |
                             QDialogButtonBox::Save | QDialogButtonBox::Discard);
    ttc::localizeStandardButtons(&buttons);

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Yes), QStringLiteral("是(&Y)"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::No), QStringLiteral("否(&N)"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Ok), QStringLiteral("确定"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("取消"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Close), QStringLiteral("关闭"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Save), QStringLiteral("保存"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Discard), QStringLiteral("丢弃"));

    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, RelocalizesExistingButtonsAfterLanguageChange) {
    switchLanguage(QStringLiteral("en"));

    QDialogButtonBox buttons(QDialogButtonBox::Yes | QDialogButtonBox::No | QDialogButtonBox::Ok |
                             QDialogButtonBox::Cancel);
    ttc::localizeStandardButtons(&buttons);
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Yes), QStringLiteral("&Yes"));

    switchLanguage(QStringLiteral("zh_CN"));

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Yes), QStringLiteral("是(&Y)"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::No), QStringLiteral("否(&N)"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Ok), QStringLiteral("确定"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("取消"));

    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, FallsBackWhenTheQtCatalogIsUnavailable) {
    switchLanguage(QStringLiteral("en"));
    qApp->setProperty("ttc.uiLanguage", QStringLiteral("zh_CN"));
    qApp->setProperty("ttc.qtBaseCatalogLoaded", false);

    QDialogButtonBox buttons(QDialogButtonBox::Yes | QDialogButtonBox::No | QDialogButtonBox::Ok |
                             QDialogButtonBox::Cancel | QDialogButtonBox::Close |
                             QDialogButtonBox::Save | QDialogButtonBox::Discard);
    ttc::localizeStandardButtons(&buttons);

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Yes), QStringLiteral("是"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::No), QStringLiteral("否"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Ok), QStringLiteral("确定"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("取消"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Close), QStringLiteral("关闭"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Save), QStringLiteral("保存"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Discard), QStringLiteral("放弃"));

    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, FallsBackWhenTheQtCatalogReturnsAnEmptyLabel) {
    switchLanguage(QStringLiteral("zh_CN"));

    QDialogButtonBox buttons(QDialogButtonBox::Ok);
    buttons.button(QDialogButtonBox::Ok)->setText(QString());
    ttc::localizeStandardButtons(&buttons);

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Ok), QStringLiteral("确定"));
    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, FontDialogRejectReturnsInitialFontAndFalse) {
    const QFont initial(QStringLiteral("Sans Serif"), 11);
    bool accepted = true;

    QTimer::singleShot(0, [] {
        for (QWidget *widget : qApp->topLevelWidgets()) {
            if (auto *fontDialog = widget->findChild<QFontDialog *>()) {
                fontDialog->reject();
                return;
            }
        }
    });

    const QFont selected = ttc::getFont(&accepted, initial, nullptr, QStringLiteral("Choose Font"));

    EXPECT_FALSE(accepted);
    EXPECT_EQ(selected, initial);
}

TEST(StandardButtonLocalizationTest, FontDialogAcceptReturnsSelectedFontAndTrue) {
    const QFont initial(QStringLiteral("Sans Serif"), 11);
    const QFont expected(QStringLiteral("Sans Serif"), 17, QFont::Bold);
    bool accepted = false;

    QTimer::singleShot(0, [&expected] {
        for (QWidget *widget : qApp->topLevelWidgets()) {
            if (auto *fontDialog = widget->findChild<QFontDialog *>()) {
                fontDialog->setCurrentFont(expected);
                fontDialog->accept();
                return;
            }
        }
    });

    const QFont selected = ttc::getFont(&accepted, initial, nullptr, QStringLiteral("Choose Font"));

    EXPECT_TRUE(accepted);
    EXPECT_EQ(selected.pointSize(), expected.pointSize());
}
