#include <gtest/gtest.h>

#include "TryUntil.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontDialog>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
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

QString normalizedButtonText(QString text) {
    text.remove(QLatin1Char('&'));
    return text;
}

QString qtStandardButtonText(QDialogButtonBox::StandardButton button) {
    QDialogButtonBox reference(button);
    return dialogButtonText(reference, button);
}

QFontDialog *activeFontDialog() {
    if (QWidget *modal = qApp->activeModalWidget()) {
        if (auto *dialog = modal->findChild<QFontDialog *>(QStringLiteral("ThemedFontDialog")))
            return dialog;
    }
    return nullptr;
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

    // Qt's catalog wins where it translated the string, and the fallback fills
    // in where it did not. Loading a catalog is not the same as translating a
    // string: qtbase_zh_CN.qm loads on this machine and returns "Cancel"
    // unchanged, which used to leave that button in English on a Chinese UI.
    const QList<QString> englishTexts = {
        QStringLiteral("&Yes"),   QStringLiteral("&No"),   QStringLiteral("OK"),
        QStringLiteral("Cancel"), QStringLiteral("Close"), QStringLiteral("Save"),
        QStringLiteral("Discard"),
    };
    const QList<QString> fallbackTexts = {
        QStringLiteral("是"), QStringLiteral("否"), QStringLiteral("确定"),
        QStringLiteral("取消"), QStringLiteral("关闭"), QStringLiteral("保存"),
        QStringLiteral("放弃"),
    };
    int index = 0;
    for (const QDialogButtonBox::StandardButton button :
         {QDialogButtonBox::Yes, QDialogButtonBox::No, QDialogButtonBox::Ok,
          QDialogButtonBox::Cancel, QDialogButtonBox::Close, QDialogButtonBox::Save,
          QDialogButtonBox::Discard}) {
        const QString qtText = qtStandardButtonText(button);
        const QString expected =
            qtText == englishTexts.at(index) ? fallbackTexts.at(index) : qtText;
        EXPECT_EQ(messageButtonText(message, static_cast<QMessageBox::StandardButton>(button)),
                  expected);
        ++index;
    }

    QDialogButtonBox buttons(QDialogButtonBox::Yes | QDialogButtonBox::No | QDialogButtonBox::Ok |
                             QDialogButtonBox::Cancel | QDialogButtonBox::Close |
                             QDialogButtonBox::Save | QDialogButtonBox::Discard);
    ttc::localizeStandardButtons(&buttons);

    index = 0;
    for (const QDialogButtonBox::StandardButton button :
         {QDialogButtonBox::Yes, QDialogButtonBox::No, QDialogButtonBox::Ok,
          QDialogButtonBox::Cancel, QDialogButtonBox::Close, QDialogButtonBox::Save,
          QDialogButtonBox::Discard}) {
        const QString qtText = qtStandardButtonText(button);
        const QString expected =
            qtText == englishTexts.at(index) ? fallbackTexts.at(index) : qtText;
        EXPECT_EQ(dialogButtonText(buttons, button), expected);
        ++index;
    }

    switchLanguage(QStringLiteral("en"));
}

TEST(StandardButtonLocalizationTest, RelocalizesExistingButtonsAfterLanguageChange) {
    switchLanguage(QStringLiteral("zh_CN"));
    QDialogButtonBox chineseReference(QDialogButtonBox::Yes | QDialogButtonBox::No |
                                      QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ttc::localizeStandardButtons(&chineseReference);
    const QList<QString> expectedChinese = {
        dialogButtonText(chineseReference, QDialogButtonBox::Yes),
        dialogButtonText(chineseReference, QDialogButtonBox::No),
        dialogButtonText(chineseReference, QDialogButtonBox::Ok),
        dialogButtonText(chineseReference, QDialogButtonBox::Cancel),
    };

    switchLanguage(QStringLiteral("en"));

    QDialogButtonBox buttons(QDialogButtonBox::Yes | QDialogButtonBox::No | QDialogButtonBox::Ok |
                             QDialogButtonBox::Cancel);
    ttc::localizeStandardButtons(&buttons);
    EXPECT_EQ(normalizedButtonText(dialogButtonText(buttons, QDialogButtonBox::Yes)),
              QStringLiteral("Yes"));

    switchLanguage(QStringLiteral("zh_CN"));

    const QList<QDialogButtonBox::StandardButton> standardButtons = {
        QDialogButtonBox::Yes,
        QDialogButtonBox::No,
        QDialogButtonBox::Ok,
        QDialogButtonBox::Cancel,
    };
    for (int index = 0; index < standardButtons.size(); ++index)
        FC_TRY_COMPARE_WITH_TIMEOUT(dialogButtonText(buttons, standardButtons.at(index)),
                                  expectedChinese.at(index), 500);

    switchLanguage(QStringLiteral("en"));
    FC_TRY_VERIFY_WITH_TIMEOUT(
        normalizedButtonText(dialogButtonText(buttons, QDialogButtonBox::Yes)) ==
            QStringLiteral("Yes"),
        500);
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
    switchLanguage(QStringLiteral("zh_CN"));
    QDialogButtonBox chineseReference(QDialogButtonBox::Cancel);
    ttc::localizeStandardButtons(&chineseReference);
    const QString expectedChineseCancel =
        dialogButtonText(chineseReference, QDialogButtonBox::Cancel);

    switchLanguage(QStringLiteral("en"));

    ConnectDialog dialog;
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    ASSERT_NE(buttons, nullptr);
    EXPECT_EQ(dialogButtonText(*buttons, QDialogButtonBox::Ok), QStringLiteral("Connect"));

    switchLanguage(QStringLiteral("zh_CN"));

    EXPECT_EQ(dialogButtonText(*buttons, QDialogButtonBox::Ok), QStringLiteral("连接"));
    FC_TRY_COMPARE_WITH_TIMEOUT(dialogButtonText(*buttons, QDialogButtonBox::Cancel),
                              expectedChineseCancel, 500);

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

    QTimer driver;
    driver.setInterval(10);
    QObject::connect(&driver, &QTimer::timeout, &driver, [&driver] {
        if (QFontDialog *fontDialog = activeFontDialog()) {
            driver.stop();
            fontDialog->reject();
        }
    });
    driver.start();
    QTimer::singleShot(2000, &driver, [&driver] {
        if (!driver.isActive())
            return;
        driver.stop();
        if (QWidget *modal = qApp->activeModalWidget())
            modal->close();
    });

    const QFont selected = ttc::getFont(&accepted, initial, nullptr, QStringLiteral("Choose Font"));
    driver.stop();

    EXPECT_FALSE(accepted);
    EXPECT_EQ(selected, initial);
}

TEST(StandardButtonLocalizationTest, FontDialogAcceptReturnsSelectedFontAndTrue) {
    const QFont initial(QStringLiteral("Sans Serif"), 11);
    const QFont expected(QStringLiteral("Sans Serif"), 17, QFont::Bold);
    bool accepted = false;
    bool selectedExpectedFont = false;
    bool foundFontDialog = false;
    QPointer<QFontDialog> drivenFontDialog;

    QTimer driver;
    driver.setInterval(10);
    QObject::connect(&driver, &QTimer::timeout, &driver,
                     [&driver, &expected, &selectedExpectedFont,
                      &drivenFontDialog, &foundFontDialog] {
        QFontDialog *fontDialog = activeFontDialog();
        if (!fontDialog)
            return;
        foundFontDialog = true;
        drivenFontDialog = fontDialog;
        selectedExpectedFont = true;
        driver.stop();
        QMetaObject::invokeMethod(fontDialog, "currentFontChanged", Qt::DirectConnection,
                                  Q_ARG(QFont, expected));
        QMetaObject::invokeMethod(fontDialog, "accepted", Qt::DirectConnection);
    });
    driver.start();
    QTimer::singleShot(2000, &driver, [&driver, &drivenFontDialog] {
        if (!driver.isActive())
            return;
        driver.stop();
        if (drivenFontDialog)
            drivenFontDialog->reject();
        else if (QWidget *modal = qApp->activeModalWidget())
            modal->close();
    });

    const QFont selected = ttc::getFont(&accepted, initial, nullptr, QStringLiteral("Choose Font"));
    driver.stop();

    EXPECT_TRUE(accepted);
    EXPECT_TRUE(foundFontDialog);
    EXPECT_TRUE(selectedExpectedFont);
    EXPECT_EQ(selected.pointSize(), expected.pointSize());
}
