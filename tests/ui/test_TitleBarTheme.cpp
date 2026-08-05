#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QScrollBar>
#include <QToolButton>
#include <QWidget>

#include "AppIcon.h"
#include "DialogTitleBar.h"
#include "FileListView.h"
#include "FramelessDialog.h"

#include "TitleBar.h"
#include "theme/ThemeManager.h"
#include "ThemeStateGuard.h"

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
#ifdef Q_OS_WIN
    // The Windows offscreen platform does not rasterize native tool-button text
    // into QWidget::grab(). Palette assertions below still exercise the theme
    // contract; exact rendering is covered on platforms where grab is reliable.
    Q_UNUSED(titleBar);
    Q_UNUSED(menuButton);
    Q_UNUSED(color);
    return;
#endif
    const QImage image = titleBar.grab().toImage();
    int matchingPixels = 0;
    const QRect bounds = menuButton.geometry().intersected(image.rect());
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (qAbs(pixel.red() - color.red()) <= 24 &&
                qAbs(pixel.green() - color.green()) <= 24 &&
                qAbs(pixel.blue() - color.blue()) <= 24)
                ++matchingPixels;
        }
    }
    EXPECT_GT(matchingPixels, 5) << "menu text is obscured by the title overlay";
}

bool imageContainsPhosphor(const QImage &image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 0 && pixel.green() > pixel.red() * 2 &&
                pixel.green() * 2 > pixel.blue() * 3)
                return true;
        }
    }
    return false;
}

} // namespace

TEST(TitleBarThemeTest, ExistingApplicationIconFollowsRuntimeCrtThemeChange) {
    ThemeStateGuard themeState;
    const QString originalSheet = qApp->styleSheet();
    const QIcon originalIcon = qApp->windowIcon();
    QWidget window;
    window.setWindowIcon(ttc::appIcon());
    TitleBar titleBar(&window, {}, &window);
    titleBar.show();
    qApp->processEvents();

    QLabel *iconLabel = nullptr;
    for (QLabel *label : titleBar.findChildren<QLabel *>()) {
        if (label->pixmap() && !label->pixmap()->isNull()) {
            iconLabel = label;
            break;
        }
    }
    ASSERT_NE(iconLabel, nullptr);
    ASSERT_FALSE(imageContainsPhosphor(iconLabel->pixmap()->toImage()));

    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Crt);
    qApp->processEvents();

    EXPECT_TRUE(imageContainsPhosphor(window.windowIcon().pixmap(32, 32).toImage()));
    ASSERT_NE(iconLabel->pixmap(), nullptr);
    EXPECT_TRUE(imageContainsPhosphor(iconLabel->pixmap()->toImage()));

    qApp->setStyleSheet(originalSheet);
    qApp->setWindowIcon(originalIcon);
}

TEST(TitleBarThemeTest, LeavingCrtClearsTheBackgroundTile) {
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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
    ThemeStateGuard themeState;
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

TEST(TitleBarThemeTest, ExistingVerticalScrollBarFollowsThemeChanges) {
    ThemeStateGuard themeState;
    const QString originalSheet = qApp->styleSheet();
    QScrollBar scrollBar(Qt::Vertical);
    scrollBar.setRange(0, 100);
    scrollBar.setPageStep(10);
    scrollBar.resize(16, 220);
    scrollBar.show();

    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Crt);
    qApp->processEvents();
    EXPECT_EQ(scrollBar.palette().color(QPalette::Window), QColor(0x04, 0x14, 0x0a));

    themeManager.apply(Settings::Theme::Dark);
    qApp->processEvents();
    EXPECT_EQ(scrollBar.palette().color(QPalette::Window), QColor(0x23, 0x23, 0x23));

    const QImage rendered = scrollBar.grab().toImage();
    int brightestPixel = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x)
            brightestPixel = qMax(brightestPixel, rendered.pixelColor(x, y).lightness());
    }
    EXPECT_LE(brightestPixel, 100) << "scrollbar retained a bright CRT/native sub-control";
    qApp->setStyleSheet(originalSheet);
}
