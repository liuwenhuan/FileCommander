#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

#include "FilePanel.h"
#include "IconFileView.h"
#include "TabBar.h"
#include "theme/ThemeManager.h"

namespace {

bool containsColorNear(const QImage &image, const QColor &expected, int tolerance = 24) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 0 && qAbs(pixel.red() - expected.red()) <= tolerance &&
                qAbs(pixel.green() - expected.green()) <= tolerance &&
                qAbs(pixel.blue() - expected.blue()) <= tolerance)
                return true;
        }
    }
    return false;
}

void applyThemeSheet(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name +
               QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

TEST(FilePanelStartupTest, DetailsModeDoesNotConstructHiddenPanelSurfaces) {
    FilePanel panel;

    EXPECT_EQ(panel.findChild<QTreeView *>(), nullptr);
    EXPECT_EQ(panel.findChild<IconFileView *>(), nullptr);
    EXPECT_EQ(panel.iconView(), nullptr);
}

TEST(FilePanelThemeTest, LazilyCreatedIconViewInheritsCurrentPalette) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};
    FilePanel panel;
    const QColor text(19, 87, 143);
    const QColor base(231, 226, 211);
    qApp->setStyleSheet(QStringLiteral(
        "IconFileView { color: rgb(19, 87, 143); background-color: rgb(231, 226, 211); }"));

    panel.toggleViewMode();
    IconFileView *iconView = panel.iconView();
    ASSERT_NE(iconView, nullptr);
    EXPECT_EQ(iconView->palette().color(QPalette::Text), text);
    EXPECT_EQ(iconView->palette().color(QPalette::Base), base);
}

TEST(FilePanelThemeTest, TreeButtonSharesTheAddressRowWithNavigationButtons) {
    FilePanel panel;

    QToolButton *addTab = panel.findChild<QToolButton *>(QStringLiteral("PanelAddTabButton"));
    QToolButton *tree = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton"));
    ASSERT_NE(addTab, nullptr);
    ASSERT_NE(tree, nullptr);

    // The selector names are a QSS contract: theme sheets may distinguish these
    // two tab-row controls without coupling to their text glyphs or construction
    // order. The actual QSS rules land with the theme work; this test preserves
    // the stable selector surface it will target.
    EXPECT_EQ(addTab->objectName(), QStringLiteral("PanelAddTabButton"));
    EXPECT_EQ(tree->objectName(), QStringLiteral("PanelTreeButton"));

    const QList<QToolButton *> buttons = panel.findChildren<QToolButton *>();
    QToolButton *back = nullptr;
    QToolButton *forward = nullptr;
    QToolButton *star = nullptr;
    for (QToolButton *button : buttons) {
        if (button->text() == QStringLiteral("←"))
            back = button;
        else if (button->text() == QStringLiteral("→"))
            forward = button;
        else if (button->text() == QStringLiteral("✳"))
            star = button;
        if (button == addTab || button == tree)
            continue;
        EXPECT_NE(button->objectName(), QStringLiteral("PanelAddTabButton"));
        EXPECT_NE(button->objectName(), QStringLiteral("PanelTreeButton"));
    }
    ASSERT_NE(back, nullptr);
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(star, nullptr);
    EXPECT_TRUE(back->objectName().isEmpty());
    EXPECT_TRUE(forward->objectName().isEmpty());
    EXPECT_TRUE(star->objectName().isEmpty());

    panel.show();
    qApp->processEvents();
    EXPECT_EQ(tree->y(), back->y());
    EXPECT_EQ(tree->size(), back->size());
    EXPECT_EQ(tree->size(), forward->size());
    EXPECT_EQ(tree->size(), star->size());
    EXPECT_EQ(addTab->width(), addTab->height());
}

TEST(FilePanelThemeTest, TreeButtonUsesCompactPaintedGlyph) {
    FilePanel panel;

    QToolButton *tree = panel.findChild<QToolButton *>(QStringLiteral("PanelTreeButton"));
    ASSERT_NE(tree, nullptr);

    EXPECT_TRUE(tree->text().isEmpty());
    EXPECT_TRUE(tree->property("compactTreeGlyph").toBool());
    EXPECT_LE(tree->property("treeGlyphMaxWidth").toInt(), 14);
}

TEST(FilePanelThemeTest, TabAndAddressRowButtonsUseTheirOwnThemeSelectors) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());
        EXPECT_TRUE(sheet.contains(QStringLiteral("QToolButton#PanelAddTabButton")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QStringLiteral("QWidget#PanelAddressRow QToolButton")))
            << theme.toStdString();
    }
}

TEST(FilePanelThemeTest, AddTabAndAddressRowControlsDefineEveryInteractionStateInEveryTheme) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());

        const QString selector = QStringLiteral("QToolButton#PanelAddTabButton");
        EXPECT_TRUE(sheet.contains(selector)) << theme.toStdString();
        EXPECT_TRUE(sheet.contains(selector + QStringLiteral(":hover")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(selector + QStringLiteral(":pressed")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(selector + QStringLiteral(":disabled")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QStringLiteral("QWidget#PanelAddressRow QToolButton:checked")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QStringLiteral("QWidget#PanelAddressRow QToolButton:checked:hover")))
            << theme.toStdString();
    }
}

TEST(FilePanelThemeTest, TabScrollerControlsDefineEveryInteractionStateInEveryTheme) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());

        const QString selector = QStringLiteral("QTabBar QToolButton");
        EXPECT_TRUE(sheet.contains(selector)) << theme.toStdString();
        EXPECT_TRUE(sheet.contains(selector + QStringLiteral(":hover")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(selector + QStringLiteral(":pressed")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(selector + QStringLiteral(":disabled")))
            << theme.toStdString();
    }
}

TEST(FilePanelThemeTest, ScrollbarsDefineStableExtentsAndHandleMinimumsInEveryTheme) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());

        EXPECT_TRUE(sheet.contains(QRegularExpression(
            QStringLiteral("QScrollBar:vertical\\s*\\{[^}]*\\bwidth\\s*:"))))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QRegularExpression(
            QStringLiteral("QScrollBar:horizontal\\s*\\{[^}]*\\bheight\\s*:"))))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QRegularExpression(
            QStringLiteral("QScrollBar::handle:vertical\\s*\\{[^}]*\\bmin-height\\s*:"))))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QRegularExpression(
            QStringLiteral("QScrollBar::handle:horizontal\\s*\\{[^}]*\\bmin-width\\s*:"))))
            << theme.toStdString();
    }
}

TEST(FilePanelThemeTest, TabBarsKeepTheSameHeightWithDifferentTabCounts) {
    QFile qss(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/dark.qss"));
    ASSERT_TRUE(qss.open(QIODevice::ReadOnly)) << "theme qss missing";
    qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    struct ClearSheet {
        ~ClearSheet() { qApp->setStyleSheet(QString()); }
    } clearSheet;

    QWidget host;
    auto *layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *singleTabPanel = new FilePanel(&host);
    auto *multipleTabPanel = new FilePanel(&host);
    layout->addWidget(singleTabPanel);
    layout->addWidget(multipleTabPanel);

    singleTabPanel->navigateTo(QDir::rootPath());
    multipleTabPanel->navigateTo(QDir::rootPath());
    multipleTabPanel->newTab();

    host.resize(1000, 700);
    host.show();
    qApp->processEvents();

    TabBar *singleTabBar = singleTabPanel->findChild<TabBar *>();
    TabBar *multipleTabBar = multipleTabPanel->findChild<TabBar *>();
    ASSERT_NE(singleTabBar, nullptr);
    ASSERT_NE(multipleTabBar, nullptr);
    EXPECT_EQ(singleTabBar->height(), multipleTabBar->height());
    EXPECT_EQ(singleTabBar->tabRect(0).height(), multipleTabBar->tabRect(0).height());
}

TEST(TabBarTest, AppliesDarkAndGreenThemesWhenStylesheetArrivesAfterConstruction) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};
    qApp->setStyleSheet(QString());

    TabBar tabBar;
    tabBar.addTab(QStringLiteral("Themed tab"));
    tabBar.resize(360, 36);
    tabBar.show();

    const auto darkSurfaceRatio = [&tabBar]() {
        const QImage rendered = tabBar.grab().toImage();
        const QRect sample = tabBar.tabRect(0).adjusted(3, 5, -3, -3);
        int dark = 0;
        int total = 0;
        for (int y = sample.top(); y <= sample.bottom(); ++y) {
            for (int x = sample.left(); x <= sample.right(); ++x) {
                ++total;
                if (rendered.pixelColor(x, y).lightness() < 128)
                    ++dark;
            }
        }
        return total > 0 ? static_cast<double>(dark) / total : 0.0;
    };

    for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("green")}) {
        QFile qss(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                  QStringLiteral(".qss"));
        ASSERT_TRUE(qss.open(QIODevice::ReadOnly)) << theme.toStdString();
        qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
        qApp->processEvents();

        EXPECT_GT(darkSurfaceRatio(), 0.65) << theme.toStdString();
    }
}

TEST(TabBarTest, CrtActiveTabAccentUsesLitPhosphor) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};

    TabBar tabBar;
    tabBar.addTab(QStringLiteral("Active tab"));
    tabBar.resize(320, 36);
    tabBar.show();
    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Crt);

    const QImage rendered = tabBar.grab().toImage();
    const QRect accentRect(tabBar.tabRect(0).x(), tabBar.tabRect(0).y(),
                           tabBar.tabRect(0).width(), 3);
    EXPECT_TRUE(containsColorNear(rendered.copy(accentRect), QColor(0x33, 0xff, 0x88), 8));
    EXPECT_FALSE(containsColorNear(rendered.copy(accentRect), QColor(0x3d, 0x7d, 0xeb), 8));
}

TEST(TabBarTest, CrtCloseButtonsUsePhosphorForEveryInteractionState) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};

    TabBar tabBar;
    tabBar.addTab(QStringLiteral("Selected"));
    tabBar.addTab(QStringLiteral("Inactive"));
    tabBar.resize(520, 36);
    tabBar.setCurrentIndex(0);
    tabBar.show();
    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Crt);

    auto *selected = qobject_cast<QAbstractButton *>(
        tabBar.tabButton(0, QTabBar::RightSide));
    auto *inactive = qobject_cast<QAbstractButton *>(
        tabBar.tabButton(1, QTabBar::RightSide));
    ASSERT_NE(selected, nullptr);
    ASSERT_NE(inactive, nullptr);

    EXPECT_TRUE(containsColorNear(selected->grab().toImage(), QColor(0x33, 0xff, 0x88)));
    EXPECT_TRUE(containsColorNear(inactive->grab().toImage(), QColor(0x1f, 0xa8, 0x5c)));

    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(inactive, &enter);
    qApp->processEvents();
    EXPECT_TRUE(containsColorNear(inactive->grab().toImage(), QColor(0x33, 0xff, 0x88)));
    EXPECT_FALSE(containsColorNear(inactive->grab().toImage(), QColor(0xe0, 0x4b, 0x4b)));

    QTest::mousePress(inactive, Qt::LeftButton, Qt::NoModifier, inactive->rect().center());
    qApp->processEvents();
    EXPECT_TRUE(containsColorNear(inactive->grab().toImage(), QColor(0x7c, 0xe8, 0xac)));
    QTest::mouseRelease(inactive, Qt::LeftButton, Qt::NoModifier,
                        inactive->rect().center());

    TabBar singleTabBar;
    singleTabBar.addTab(QStringLiteral("Only tab"));
    singleTabBar.resize(260, 36);
    singleTabBar.show();
    qApp->processEvents();
    auto *disabled = qobject_cast<QAbstractButton *>(
        singleTabBar.tabButton(0, QTabBar::RightSide));
    ASSERT_NE(disabled, nullptr);
    EXPECT_FALSE(disabled->isEnabled());
    EXPECT_TRUE(containsColorNear(disabled->grab().toImage(), QColor(0x12, 0x60, 0x2f)));
}

TEST(TabBarTest, LightAndDarkKeepTheirExistingAccentAndCloseColours) {
    const QString previousStyleSheet = qApp->styleSheet();
    struct RestoreStyleSheet {
        QString value;
        ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
    } restore{previousStyleSheet};

    struct ThemeColours {
        QString name;
        QColor accent;
        QColor close;
        QColor disabled;
    };
    // The dark accent is a neutral slate, not the light theme's blue: its file
    // grid is greyscale once the icons follow the theme, and a saturated accent
    // was the only coloured thing on screen. See the note atop dark.qss.
    const QList<ThemeColours> cases = {
        {QStringLiteral("light"), QColor(0x3d, 0x7d, 0xeb), QColor(0x20, 0x20, 0x20),
         QColor(0xa0, 0xa0, 0xa0)},
        {QStringLiteral("dark"), QColor(0x8e, 0x94, 0x9c), QColor(0xe0, 0xe0, 0xe0),
         QColor(0x77, 0x77, 0x77)},
    };

    for (const ThemeColours &theme : cases) {
        TabBar tabBar;
        tabBar.addTab(QStringLiteral("Selected"));
        tabBar.addTab(QStringLiteral("Inactive"));
        tabBar.resize(520, 36);
        tabBar.setCurrentIndex(0);
        tabBar.show();
        ThemeManager themeManager;
        themeManager.apply(theme.name == QStringLiteral("light")
                               ? Settings::Theme::Light
                               : Settings::Theme::Dark);

        const QImage rendered = tabBar.grab().toImage();
        const QRect accentRect(tabBar.tabRect(0).x(), tabBar.tabRect(0).y(),
                               tabBar.tabRect(0).width(), 3);
        EXPECT_TRUE(containsColorNear(rendered.copy(accentRect), theme.accent, 8))
            << theme.name.toStdString();

        auto *selected = qobject_cast<QAbstractButton *>(
            tabBar.tabButton(0, QTabBar::RightSide));
        auto *inactive = qobject_cast<QAbstractButton *>(
            tabBar.tabButton(1, QTabBar::RightSide));
        ASSERT_NE(selected, nullptr);
        ASSERT_NE(inactive, nullptr);
        EXPECT_TRUE(containsColorNear(selected->grab().toImage(), theme.close))
            << theme.name.toStdString();

        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(inactive, &enter);
        qApp->processEvents();
        EXPECT_TRUE(containsColorNear(inactive->grab().toImage(), QColor(0xe0, 0x4b, 0x4b)))
            << theme.name.toStdString();

        TabBar singleTabBar;
        singleTabBar.addTab(QStringLiteral("Only tab"));
        singleTabBar.resize(260, 36);
        singleTabBar.show();
        qApp->processEvents();
        auto *disabled = qobject_cast<QAbstractButton *>(
            singleTabBar.tabButton(0, QTabBar::RightSide));
        ASSERT_NE(disabled, nullptr);
        EXPECT_FALSE(disabled->isEnabled());
        EXPECT_TRUE(containsColorNear(disabled->grab().toImage(), theme.disabled))
            << theme.name.toStdString();
    }
}

TEST(FilePanelThemeTest, MotionProgressPropertiesRemainAvailableToThemeAwarePainting) {
    FilePanel panel;
    TabBar *tabBar = panel.findChild<TabBar *>();
    ASSERT_NE(tabBar, nullptr);

    EXPECT_GE(panel.metaObject()->indexOfProperty("focusProgress"), 0);
    EXPECT_GE(tabBar->metaObject()->indexOfProperty("activationProgress"), 0);
}

TEST(TabBarTest, OverflowUsesNativeScrollersInsideTheTabBar) {
    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Dark);
    TabBar tabBar;
    for (int index = 0; index < 20; ++index)
        tabBar.addTab(QStringLiteral("Long tab title %1").arg(index));

    tabBar.resize(180, 36);
    tabBar.setCurrentIndex(tabBar.count() - 1);
    tabBar.show();
    qApp->processEvents();
    qApp->processEvents();

    QToolButton *left = nullptr;
    QToolButton *right = nullptr;
    for (QToolButton *button :
         tabBar.findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button->arrowType() == Qt::LeftArrow)
            left = button;
        else if (button->arrowType() == Qt::RightArrow)
            right = button;
    }

    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_TRUE(left->isVisible());
    EXPECT_TRUE(right->isVisible());
    int maxTabRight = -1;
    for (int index = 0; index < tabBar.count(); ++index) {
        const QRect tab = tabBar.tabRect(index);
        if (tab.isValid() && tab.left() < right->geometry().left())
            maxTabRight = qMax(maxTabRight, tab.right());
    }
    EXPECT_LE(left->geometry().left(), 1);
    EXPECT_GE(right->geometry().right(), tabBar.width() - 2);
    EXPECT_LT(left->geometry().right(), right->geometry().left());
    EXPECT_GE(maxTabRight, right->geometry().left() - 1);
    const QImage rendered = tabBar.grab().toImage();
    // The strip's accent, not the palette's highlight. They were the same colour
    // until the dark theme took a neutral accent: the highlight is a FILL that
    // has to carry white text (#565b63), the accent is a LINE that has to read
    // against #2b2b2b (#8e949c). See the note atop dark.qss.
    const QColor accent(0x8e, 0x94, 0x9c);
    bool accentBeforeRight = false;
    for (int x = qMax(left->geometry().right() + 1, right->geometry().left() - 20);
         x < right->geometry().left(); ++x) {
        for (int y = 0; y < qMin(4, rendered.height()); ++y) {
            const QColor pixel = rendered.pixelColor(x, y);
            if (qAbs(pixel.red() - accent.red()) < 8 &&
                qAbs(pixel.green() - accent.green()) < 8 &&
                qAbs(pixel.blue() - accent.blue()) < 8) {
                accentBeforeRight = true;
            }
        }
    }
    EXPECT_TRUE(accentBeforeRight);
}

TEST(TabBarTest, OverflowAccentDoesNotPaintUnderNativeScrollers) {
    ThemeManager themeManager;
    themeManager.apply(Settings::Theme::Dark);
    TabBar tabBar;
    for (int index = 0; index < 20; ++index)
        tabBar.addTab(QStringLiteral("Long tab title %1").arg(index));

    tabBar.resize(240, 36);
    tabBar.setCurrentIndex(tabBar.count() - 1);
    tabBar.show();
    qApp->processEvents();
    qApp->processEvents();

    QToolButton *left = nullptr;
    QToolButton *right = nullptr;
    for (QToolButton *button :
         tabBar.findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button->arrowType() == Qt::LeftArrow)
            left = button;
        else if (button->arrowType() == Qt::RightArrow)
            right = button;
    }

    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(left->isVisible());
    ASSERT_TRUE(right->isVisible());

    const QImage rendered = tabBar.grab().toImage();
    // The strip's accent, not the palette's highlight. They were the same colour
    // until the dark theme took a neutral accent: the highlight is a FILL that
    // has to carry white text (#565b63), the accent is a LINE that has to read
    // against #2b2b2b (#8e949c). See the note atop dark.qss.
    const QColor accent(0x8e, 0x94, 0x9c);
    auto containsAccent = [&](const QRect &rect) {
        for (int x = rect.left(); x <= rect.right() && x < rendered.width(); ++x) {
            for (int y = rect.top(); y <= qMin(rect.bottom(), 3); ++y) {
                const QColor pixel = rendered.pixelColor(x, y);
                if (qAbs(pixel.red() - accent.red()) < 8 &&
                    qAbs(pixel.green() - accent.green()) < 8 &&
                    qAbs(pixel.blue() - accent.blue()) < 8)
                    return true;
            }
        }
        return false;
    };

    EXPECT_FALSE(containsAccent(left->geometry()));
    EXPECT_FALSE(containsAccent(right->geometry()));
}

TEST(TabBarTest, OverflowScrollersReflectAvailableDirection) {
    TabBar tabBar;
    for (int index = 0; index < 20; ++index)
        tabBar.addTab(QStringLiteral("Long tab title %1").arg(index));

    tabBar.resize(180, 36);
    tabBar.show();
    qApp->processEvents();
    qApp->processEvents();

    QToolButton *left = nullptr;
    QToolButton *right = nullptr;
    for (QToolButton *button :
         tabBar.findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button->arrowType() == Qt::LeftArrow)
            left = button;
        else if (button->arrowType() == Qt::RightArrow)
            right = button;
    }

    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    tabBar.setCurrentIndex(0);
    qApp->processEvents();
    EXPECT_FALSE(left->isEnabled());
    EXPECT_TRUE(right->isEnabled());

    tabBar.setCurrentIndex(tabBar.count() - 1);
    qApp->processEvents();
    qApp->processEvents();
    EXPECT_TRUE(left->isEnabled());
    EXPECT_FALSE(right->isEnabled());
}

TEST(FilePanelThemeTest, OverflowDoesNotCreateACustomLeftScrollControl) {
    FilePanel panel;
    panel.resize(300, 600);
    panel.show();
    qApp->processEvents();

    TabBar *tabBar = panel.findChild<TabBar *>();
    ASSERT_NE(tabBar, nullptr);
    for (int index = 0; index < 12; ++index)
        panel.newTab();
    tabBar->setCurrentIndex(0);
    qApp->processEvents();
    qApp->processEvents(); // QTabBar lays out its native buttons asynchronously.

    EXPECT_EQ(panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollLeftButton")),
              nullptr);
    EXPECT_EQ(panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollRightButton")),
              nullptr);

    QToolButton *nativeLeft = nullptr;
    QToolButton *nativeRight = nullptr;
    for (QToolButton *button :
         tabBar->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button->arrowType() == Qt::LeftArrow)
            nativeLeft = button;
        else if (button->arrowType() == Qt::RightArrow)
            nativeRight = button;
    }
    ASSERT_NE(nativeLeft, nullptr);
    ASSERT_NE(nativeRight, nullptr);
    EXPECT_TRUE(nativeLeft->isVisible());
    EXPECT_TRUE(nativeRight->isVisible());
    int maxTabRight = -1;
    for (int index = 0; index < tabBar->count(); ++index) {
        const QRect tab = tabBar->tabRect(index);
        if (tab.isValid() && tab.left() < nativeRight->geometry().left())
            maxTabRight = qMax(maxTabRight, tab.right());
    }
    EXPECT_LE(nativeLeft->geometry().left(), 1);
    EXPECT_GE(nativeRight->geometry().right(), tabBar->width() - 2);
    EXPECT_LT(nativeLeft->geometry().right(), nativeRight->geometry().left());
    EXPECT_GE(maxTabRight, nativeRight->geometry().left() - 1);
}

TEST(FilePanelThemeTest, NoCustomLeftScrollControlIsCreatedWithoutOverflow) {
    FilePanel panel;
    panel.resize(900, 600);
    panel.show();
    qApp->processEvents();

    EXPECT_EQ(panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollLeftButton")),
              nullptr);
    EXPECT_EQ(panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollRightButton")),
              nullptr);
}

} // namespace

TEST(TabBarTest, DoubleClickingATabTitleRequestsThatTabClose) {
    TabBar tabBar;
    tabBar.addTab(QStringLiteral("First"));
    tabBar.addTab(QStringLiteral("Second"));
    tabBar.resize(420, 40);
    tabBar.show();
    qApp->processEvents();

    QSignalSpy closeRequested(&tabBar, &TabBar::closeTabRequested);
    const QPoint titlePoint = tabBar.tabRect(1).center();
    ASSERT_EQ(tabBar.tabAt(titlePoint), 1);

    QTest::mouseDClick(&tabBar, Qt::LeftButton, Qt::NoModifier, titlePoint);

    ASSERT_EQ(closeRequested.count(), 1);
    EXPECT_EQ(closeRequested.first().first().toInt(), 1);
}
