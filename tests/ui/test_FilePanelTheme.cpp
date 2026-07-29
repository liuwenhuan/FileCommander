#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QToolButton>

#include "FilePanel.h"

namespace {

TEST(FilePanelThemeTest, TabAndTreeButtonsExposeDedicatedThemeSelectors) {
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
    EXPECT_EQ(addTab->size(), tree->size());
    EXPECT_EQ(addTab->width(), addTab->height());
}

TEST(FilePanelThemeTest, DedicatedTabRowButtonsAreBorderlessInEveryTheme) {
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark"),
                                 QStringLiteral("green")}) {
        QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + theme +
                   QStringLiteral(".qss"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
            << theme.toStdString();
        const QString sheet = QString::fromUtf8(file.readAll());
        EXPECT_TRUE(sheet.contains(QStringLiteral("QToolButton#PanelAddTabButton")))
            << theme.toStdString();
        EXPECT_TRUE(sheet.contains(QStringLiteral("QToolButton#PanelTreeButton")))
            << theme.toStdString();
    }
}

} // namespace
