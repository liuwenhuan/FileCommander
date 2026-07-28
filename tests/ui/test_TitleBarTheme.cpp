#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QToolButton>
#include <QWidget>

#include "DialogTitleBar.h"
#include "FileListView.h"
#include "FramelessDialog.h"

#include "TitleBar.h"
#include "theme/ThemeManager.h"

namespace {

void applyThemeSheet(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name + QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

void expectHeaderColors(FileListView &view, const QColor &background, const QColor &foreground,
                        const QColor &border) {
    QHeaderView *header = view.horizontalHeader();
    EXPECT_EQ(header->property("sectionBackground").value<QColor>(), background);
    EXPECT_EQ(header->property("sectionForeground").value<QColor>(), foreground);
    EXPECT_EQ(header->property("sectionBorder").value<QColor>(), border);
}

void expectMenuTextRendered(TitleBar &titleBar, QToolButton &menuButton, const QColor &color) {
    const QImage image = titleBar.grab().toImage();
    int matchingPixels = 0;
    const QRect bounds = menuButton.geometry().intersected(image.rect());
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (qAbs(pixel.red() - color.red()) <= 3 && qAbs(pixel.green() - color.green()) <= 3
                && qAbs(pixel.blue() - color.blue()) <= 3)
                ++matchingPixels;
        }
    }
    EXPECT_GT(matchingPixels, 5) << "menu text is obscured by the title overlay";
}

} // namespace

TEST(TitleBarThemeTest, LeavingCrtClearsTheBackgroundTile) {
    const QString originalSheet = qApp->styleSheet();
    QWidget window;
    TitleBar titleBar(&window, {}, &window);

    QPixmap crtTile(2, 2);
    crtTile.fill(Qt::green);
    titleBar.setBackgroundTile(crtTile);
    ASSERT_FALSE(titleBar.backgroundTile().isNull());

    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Light);

    EXPECT_TRUE(titleBar.backgroundTile().isNull());
    qApp->setStyleSheet(originalSheet);
}

TEST(TitleBarThemeTest, DialogTitleBarSupportsCrtTileAndClearsItOutsideCrt) {
    const QString originalSheet = qApp->styleSheet();
    FramelessDialog dialog;
    DialogTitleBar *titleBar = dialog.findChild<DialogTitleBar *>();
    ASSERT_NE(titleBar, nullptr);

    QPixmap crtTile(2, 2);
    crtTile.fill(Qt::green);
    dialog.setBackgroundTile(crtTile);
    ASSERT_FALSE(dialog.backgroundTile().isNull());
    ASSERT_FALSE(titleBar->backgroundTile().isNull());

    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Dark);

    EXPECT_TRUE(dialog.backgroundTile().isNull());
    EXPECT_TRUE(titleBar->backgroundTile().isNull());
    qApp->setStyleSheet(originalSheet);
}

TEST(TitleBarThemeTest, DialogThemeSwitchSetsAndClearsCrtTileProperty) {
    const QString originalSheet = qApp->styleSheet();
    FramelessDialog dialog;
    DialogTitleBar *titleBar = dialog.findChild<DialogTitleBar *>();
    ASSERT_NE(titleBar, nullptr);
    ThemeManager themeManager;
    dialog.show();
    qApp->processEvents();

    themeManager.apply(Settings::Theme::Crt);
    qApp->processEvents();
    ASSERT_FALSE(dialog.backgroundTile().isNull());
    EXPECT_FALSE(titleBar->backgroundTile().isNull());

    themeManager.apply(Settings::Theme::Dark);
    EXPECT_TRUE(dialog.backgroundTile().isNull());
    EXPECT_TRUE(titleBar->backgroundTile().isNull());

    themeManager.apply(Settings::Theme::Crt);
    ASSERT_FALSE(dialog.backgroundTile().isNull());
    themeManager.apply(Settings::Theme::Light);
    EXPECT_TRUE(dialog.backgroundTile().isNull());
    EXPECT_TRUE(titleBar->backgroundTile().isNull());

    qApp->setStyleSheet(originalSheet);
}

TEST(TitleBarThemeTest, LightAndDarkThemesResetMenuButtonsAndColumnHeaders) {
    const QString originalSheet = qApp->styleSheet();
    QWidget window;
    window.resize(480, 40);
    QMenu interfaceMenu(QStringLiteral("Interface"), &window);
    TitleBar titleBar(&window, {&interfaceMenu}, &window);
    titleBar.resize(window.size());
    titleBar.show();
    FileListView view;

    applyThemeSheet(QStringLiteral("green"));
    QToolButton *menuButton = titleBar.findChild<QToolButton *>(QStringLiteral("TitleMenuButton"));
    ASSERT_NE(menuButton, nullptr);
    EXPECT_EQ(menuButton->palette().color(QPalette::ButtonText), QColor(0x33, 0xff, 0x88));
    expectMenuTextRendered(titleBar, *menuButton, QColor(0x33, 0xff, 0x88));
    expectHeaderColors(view, QColor(0x0a, 0x1a, 0x0d), QColor(0x33, 0xff, 0x88),
                       QColor(0x1f, 0xa8, 0x5c));

    applyThemeSheet(QStringLiteral("light"));
    EXPECT_EQ(menuButton->palette().color(QPalette::ButtonText), QColor(0x20, 0x20, 0x20));
    expectMenuTextRendered(titleBar, *menuButton, QColor(0x20, 0x20, 0x20));
    expectHeaderColors(view, QColor(0xec, 0xec, 0xec), QColor(0x20, 0x20, 0x20),
                       QColor(0xd0, 0xd0, 0xd0));

    applyThemeSheet(QStringLiteral("dark"));
    EXPECT_EQ(menuButton->palette().color(QPalette::ButtonText), QColor(0xe0, 0xe0, 0xe0));
    expectMenuTextRendered(titleBar, *menuButton, QColor(0xe0, 0xe0, 0xe0));
    expectHeaderColors(view, QColor(0x23, 0x23, 0x23), QColor(0xe0, 0xe0, 0xe0),
                       QColor(0x50, 0x50, 0x50));
    qApp->setStyleSheet(originalSheet);
}
