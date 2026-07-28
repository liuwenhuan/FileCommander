#include <gtest/gtest.h>

#include <QPixmap>
#include <QWidget>

#include "TitleBar.h"
#include "theme/ThemeManager.h"

TEST(TitleBarThemeTest, LeavingCrtClearsTheBackgroundTile) {
    QWidget window;
    TitleBar titleBar(&window, {}, &window);

    QPixmap crtTile(2, 2);
    crtTile.fill(Qt::green);
    titleBar.setBackgroundTile(crtTile);
    ASSERT_FALSE(titleBar.backgroundTile().isNull());

    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Light);

    EXPECT_TRUE(titleBar.backgroundTile().isNull());
}
