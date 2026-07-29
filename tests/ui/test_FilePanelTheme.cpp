#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QToolButton>

#include "FilePanel.h"
#include "TabBar.h"

namespace {

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

TEST(FilePanelThemeTest, TabAndAddressRowControlsDefineEveryInteractionStateInEveryTheme) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());

        const QString selector =
            QStringLiteral("QToolButton#PanelTabScrollLeftButton");
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

TEST(TabBarTest, OverflowKeepsOnlyTheNativeRightScrollerInTheTabBar) {
    TabBar tabBar;
    for (int index = 0; index < 20; ++index)
        tabBar.addTab(QStringLiteral("Long tab title %1").arg(index));

    tabBar.resize(180, 36);
    tabBar.show();
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
    EXPECT_FALSE(left->isVisible());
    EXPECT_TRUE(right->isVisible());
}

TEST(FilePanelThemeTest, OverflowUsesCustomLeftControlAtPhysicalLeftEdge) {
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

    QToolButton *outerLeft =
        panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollLeftButton"));
    QToolButton *outerRight =
        panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollRightButton"));
    ASSERT_NE(outerLeft, nullptr);
    EXPECT_EQ(outerRight, nullptr);
    EXPECT_TRUE(outerLeft->isVisible());
    EXPECT_LT(outerLeft->geometry().right(), tabBar->geometry().left());
    EXPECT_EQ(outerLeft->arrowType(), Qt::LeftArrow);
}

TEST(FilePanelThemeTest, CustomLeftScrollControlStaysHiddenWithoutOverflow) {
    FilePanel panel;
    panel.resize(900, 600);
    panel.show();
    qApp->processEvents();

    QToolButton *outerLeft =
        panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollLeftButton"));
    QToolButton *outerRight =
        panel.findChild<QToolButton *>(QStringLiteral("PanelTabScrollRightButton"));
    ASSERT_NE(outerLeft, nullptr);
    EXPECT_EQ(outerRight, nullptr);
    EXPECT_FALSE(outerLeft->isVisible());
}

} // namespace
