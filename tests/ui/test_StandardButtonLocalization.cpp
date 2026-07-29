#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontDialog>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "ConnectDialog.h"
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

QColor mostOpaqueColor(const QIcon &icon) {
    const QImage image = icon.pixmap(16, 16).toImage().convertToFormat(QImage::Format_ARGB32);
    QColor best;
    int greatestAlpha = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > greatestAlpha) {
                best = color;
                greatestAlpha = color.alpha();
            }
        }
    }
    return best;
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

TEST(StandardButtonLocalizationTest, PreservesExplicitCanonicalEnglishOverrideAcrossLanguageChange) {
    switchLanguage(QStringLiteral("en"));
    qApp->setProperty("ttc.uiLanguage", QStringLiteral("zh_CN"));
    qApp->setProperty("ttc.qtBaseCatalogLoaded", false);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ttc::setStandardButtonOverride(buttons.button(QDialogButtonBox::Cancel),
                                   QStringLiteral("Cancel"));
    ttc::localizeStandardButtons(&buttons);

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Ok), QStringLiteral("确定"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("Cancel"));

    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&buttons, &languageChange);
    QTest::qWait(20);

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Ok), QStringLiteral("确定"));
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("Cancel"));
    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, PreservesExplicitCanonicalEnglishOverrideSetAfterFallback) {
    switchLanguage(QStringLiteral("en"));
    qApp->setProperty("ttc.uiLanguage", QStringLiteral("zh_CN"));
    qApp->setProperty("ttc.qtBaseCatalogLoaded", false);

    QDialogButtonBox buttons(QDialogButtonBox::Cancel);
    ttc::localizeStandardButtons(&buttons);
    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("取消"));

    buttons.button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&buttons, &languageChange);
    qApp->processEvents();

    EXPECT_EQ(dialogButtonText(buttons, QDialogButtonBox::Cancel), QStringLiteral("Cancel"));
    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, RelocalizesConnectOverrideAfterLanguageChange) {
    switchLanguage(QStringLiteral("en"));

    ConnectDialog dialog;
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttons, nullptr);
    EXPECT_EQ(dialogButtonText(*buttons, QDialogButtonBox::Ok), QStringLiteral("Connect"));

    switchLanguage(QStringLiteral("zh_CN"));
    QTest::qWait(20);

    EXPECT_EQ(dialogButtonText(*buttons, QDialogButtonBox::Ok), QStringLiteral("连接"));
    EXPECT_EQ(dialogButtonText(*buttons, QDialogButtonBox::Cancel), QStringLiteral("取消"));

    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, GivesStandardButtonsPaletteAwareProjectIcons) {
    const QString originalSheet = qApp->styleSheet();
    qApp->setStyleSheet(QString());

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::No |
                             QDialogButtonBox::Cancel | QDialogButtonBox::Save |
                             QDialogButtonBox::Retry | QDialogButtonBox::Help);
    QPalette palette = buttons.palette();
    palette.setColor(QPalette::ButtonText, QColor(17, 101, 203));
    buttons.setPalette(palette);
    ttc::localizeStandardButtons(&buttons);

    for (QDialogButtonBox::StandardButton standard :
         {QDialogButtonBox::Ok, QDialogButtonBox::No, QDialogButtonBox::Cancel,
          QDialogButtonBox::Save, QDialogButtonBox::Retry, QDialogButtonBox::Help}) {
        QPushButton *button = buttons.button(standard);
        ASSERT_NE(button, nullptr);
        EXPECT_FALSE(button->icon().isNull());
        EXPECT_EQ(mostOpaqueColor(button->icon()), QColor(17, 101, 203));
    }

    palette.setColor(QPalette::ButtonText, QColor(51, 255, 136));
    buttons.setPalette(palette);
    QEvent paletteChange(QEvent::PaletteChange);
    QCoreApplication::sendEvent(&buttons, &paletteChange);
    QTest::qWait(20);
    EXPECT_EQ(mostOpaqueColor(buttons.button(QDialogButtonBox::Ok)->icon()),
              QColor(51, 255, 136));

    qApp->setStyleSheet(originalSheet);
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
