#include <gtest/gtest.h>

#include <QFont>
#include <QTemporaryDir>

#include "CommandBar.h"
#include "FunctionKeyBar.h"
#include "Settings.h"
#include "StatusBarWidget.h"
#include "Typography.h"

namespace {

TEST(ChromeTypographyTest, DefaultMenuFontUsesTwelvePoints) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));

    EXPECT_EQ(Typography::chromeFont(settings).pointSize(), 12);
}

TEST(ChromeTypographyTest, MenuFontSizeAppliesToCompositeChromeWidgets) {
    QTemporaryDir temporaryDir;
    ASSERT_TRUE(temporaryDir.isValid());
    Settings settings(temporaryDir.filePath(QStringLiteral("settings.ini")));
    settings.setMenuFontSize(13);

    const QFont chrome = Typography::chromeFont(settings);
    EXPECT_EQ(chrome.pointSize(), 13);

    StatusBarWidget statusBar;
    CommandBar commandBar;
    FunctionKeyBar functionKeyBar;
    Typography::applyChromeFont(&statusBar, settings);
    Typography::applyChromeFont(&commandBar, settings);
    Typography::applyChromeFont(&functionKeyBar, settings);

    EXPECT_EQ(statusBar.font().pointSize(), 13);
    EXPECT_EQ(commandBar.font().pointSize(), 13);
    EXPECT_EQ(functionKeyBar.font().pointSize(), 13);
}

} // namespace
