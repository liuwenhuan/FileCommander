#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QFont>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QSet>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>
#include <QWidgetAction>

#include "CommandBar.h"
#include "DialogTitleBar.h"
#include "FramelessDialog.h"
#include "FunctionKeyBar.h"
#include "MainWindow.h"
#include "Settings.h"
#include "StatusBarWidget.h"
#include "Typography.h"

namespace {

class FontChangeCounter final : public QObject {
public:
    int changes = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::FontChange)
            ++changes;
        return false;
    }
};

class ApplicationAppearanceGuard final {
public:
    ApplicationAppearanceGuard()
        : m_font(QApplication::font()), m_styleSheet(qApp->styleSheet()) {}

    ~ApplicationAppearanceGuard() {
        QApplication::setFont(m_font);
        qApp->setStyleSheet(m_styleSheet);
    }

private:
    QFont m_font;
    QString m_styleSheet;
};

class EnvironmentGuard final {
public:
    EnvironmentGuard(const char *name, const QByteArray &value)
        : m_name(name), m_original(qgetenv(name)), m_hadOriginal(qEnvironmentVariableIsSet(name)) {
        qputenv(name, value);
    }

    ~EnvironmentGuard() {
        if (m_hadOriginal)
            qputenv(m_name.constData(), m_original);
        else
            qunsetenv(m_name.constData());
    }

private:
    QByteArray m_name;
    QByteArray m_original;
    bool m_hadOriginal;
};

QMenu *interfaceMenu(MainWindow &window) {
    for (QMenu *menu : window.findChildren<QMenu *>()) {
        if (menu->title() == QStringLiteral("&Interface"))
            return menu;
    }
    return nullptr;
}

QWidget *fontRow(QMenu *menu, const QString &captionText) {
    if (!menu)
        return nullptr;
    for (QWidgetAction *action : menu->findChildren<QWidgetAction *>()) {
        QWidget *row = action->defaultWidget();
        if (!row)
            continue;
        for (QLabel *caption : row->findChildren<QLabel *>()) {
            if (caption->text() == captionText)
                return row;
        }
    }
    return nullptr;
}

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

TEST(ChromeTypographyTest, ApplyingAnEqualResolvedFontDoesNotRepolishTheWidget) {
    QWidget widget;
    const QFont font = widget.font();
    FontChangeCounter counter;
    widget.installEventFilter(&counter);

    Typography::applyChromeFont(&widget, font);
    EXPECT_EQ(counter.changes, 0);

    QFont changed = font;
    changed.setPointSize(font.pointSize() + 1);
    Typography::applyChromeFont(&widget, changed);
    EXPECT_EQ(counter.changes, 1);
}

TEST(ChromeTypographyTest, EmbeddedMenuChromeTracksParentMenuFontChanges) {
    ApplicationAppearanceGuard guard;
    QFont initial = QApplication::font();
    initial.setPointSize(10);
    QApplication::setFont(initial);

    QMenu menu;
    Typography::applyChromeFont(&menu, initial);
    auto *row = new QWidget(&menu);
    auto *layout = new QHBoxLayout(row);
    auto *child = new QLabel(QStringLiteral("Menu Font Size:"), row);
    layout->addWidget(child);
    Typography::applyChromeFont(row, initial);
    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(row);
    menu.addAction(action);
    menu.show();
    QApplication::processEvents();
    const int initialTextHeight = child->fontMetrics().height();

    QFont changed = initial;
    changed.setPointSize(15);
    menu.setFont(changed);
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_EQ(row->font().pointSize(), 15);
    EXPECT_EQ(child->font().pointSize(), 15);
    EXPECT_GT(child->fontMetrics().height(), initialTextHeight);
    EXPECT_GE(menu.actionGeometry(action).height(), row->sizeHint().height());
}

TEST(ChromeTypographyTest, FramelessDialogChromeTracksRuntimeApplicationFontChanges) {
    ApplicationAppearanceGuard guard;
    QFont initial = QApplication::font();
    initial.setPointSize(10);
    QApplication::setFont(initial);

    FramelessDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Large dialog title"));
    dialog.resize(360, 220);
    dialog.show();
    QApplication::processEvents();

    DialogTitleBar *titleBar = dialog.findChild<DialogTitleBar *>();
    ASSERT_NE(titleBar, nullptr);
    QAbstractButton *closeButton = titleBar->findChild<QAbstractButton *>();
    ASSERT_NE(closeButton, nullptr);
    EXPECT_EQ(titleBar->height(), 30);
    EXPECT_EQ(closeButton->height(), 30);
    EXPECT_EQ(dialog.contentsMargins().top(), 16 + titleBar->height());

    QFont large = initial;
    large.setPointSize(22);
    QApplication::setFont(large);
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_GT(titleBar->height(), 30);
    EXPECT_GE(titleBar->height(), titleBar->fontMetrics().height());
    EXPECT_EQ(closeButton->height(), titleBar->height());
    EXPECT_EQ(dialog.contentsMargins().top(), 16 + titleBar->height());
    EXPECT_EQ(titleBar->geometry(),
              QRect(16, 16, dialog.width() - 32, titleBar->height()));
    EXPECT_EQ(dialog.contentsRect().top(), titleBar->geometry().bottom() + 1);
}

TEST(ChromeTypographyTest, MainWindowFontRowsTrackTheLiveMenuFontSetting) {
    ApplicationAppearanceGuard appearanceGuard;
    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    EnvironmentGuard configGuard("FILECOMMANDER_CONFIG_HOME", configHome.path().toUtf8());
    {
        Settings settings;
        settings.setMenuFontSize(10);
    }

    MainWindow window;
    QMenu *menu = interfaceMenu(window);
    ASSERT_NE(menu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
    QWidget *menuFontRow = fontRow(menu, QStringLiteral("Menu Font Size:"));
    ASSERT_NE(menuFontRow, nullptr);
    EXPECT_EQ(menuFontRow->font().pointSize(), 10);

    QToolButton *plus = nullptr;
    for (QToolButton *button : menuFontRow->findChildren<QToolButton *>()) {
        if (button->text() == QStringLiteral("+")) {
            plus = button;
            break;
        }
    }
    ASSERT_NE(plus, nullptr);
    plus->click();
    QApplication::processEvents();

    menu = interfaceMenu(window);
    ASSERT_NE(menu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
    QWidget *fileFontRow = fontRow(menu, QStringLiteral("File List Font Size:"));
    menuFontRow = fontRow(menu, QStringLiteral("Menu Font Size:"));
    ASSERT_NE(fileFontRow, nullptr);
    ASSERT_NE(menuFontRow, nullptr);
    EXPECT_EQ(menu->font().pointSize(), 11);
    EXPECT_EQ(fileFontRow->font().pointSize(), 11);
    EXPECT_EQ(menuFontRow->font().pointSize(), 11);
}

TEST(ChromeTypographyTest, EmbeddedMenuChromeDoesNotCoverCrtScanlines) {
    ApplicationAppearanceGuard guard;
    QFile theme(QStringLiteral(":/themes/green.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));

    QMenu menu;
    auto *row = new QWidget(&menu);
    row->setFixedSize(120, 30);
    Typography::applyChromeFont(row, QApplication::font());
    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(row);
    menu.addAction(action);

    menu.ensurePolished();
    menu.resize(menu.sizeHint());
    menu.show();
    QApplication::processEvents();

    QImage image(menu.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    menu.render(&image);

    const QRect rowRect = row->geometry();
    ASSERT_GT(rowRect.width(), 8);
    ASSERT_GT(rowRect.height(), 8);
    QSet<QRgb> verticalColors;
    const int sampleX = rowRect.left() + 4;
    for (int y = rowRect.top() + 3; y < rowRect.bottom() - 3; ++y)
        verticalColors.insert(image.pixel(sampleX, y));

    EXPECT_GE(verticalColors.size(), 2);
}

// The title bar's menu buttons (Interface / Tools / Config) and the F3-F8 row
// were both left at their startup size by a menu-font change: the title bar was
// never reached from applyInterfaceTypography() at all, and the function-key bar
// was reached but only at the container level, whose own font already matched
// the just-applied application font -- so setFont() was skipped as a no-op and
// the buttons inside were never told anything had changed.
TEST(ChromeTypographyTest, TitleBarAndFunctionKeyButtonsTrackTheLiveMenuFontSetting) {
    ApplicationAppearanceGuard appearanceGuard;
    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    EnvironmentGuard configGuard("FILECOMMANDER_CONFIG_HOME", configHome.path().toUtf8());
    {
        Settings settings;
        settings.setMenuFontSize(10);
    }

    MainWindow window;

    QList<QToolButton *> titleMenuButtons;
    for (QToolButton *button : window.findChildren<QToolButton *>()) {
        if (button->objectName() == QStringLiteral("TitleMenuButton"))
            titleMenuButtons.append(button);
    }
    ASSERT_FALSE(titleMenuButtons.isEmpty());

    auto *functionKeyBar = window.findChild<FunctionKeyBar *>();
    ASSERT_NE(functionKeyBar, nullptr);
    const QList<QAbstractButton *> fnButtons = functionKeyBar->findChildren<QAbstractButton *>();
    ASSERT_FALSE(fnButtons.isEmpty());

    for (QToolButton *button : titleMenuButtons)
        EXPECT_EQ(button->font().pointSize(), 10) << button->text().toStdString();
    for (QAbstractButton *button : fnButtons)
        EXPECT_EQ(button->font().pointSize(), 10) << button->text().toStdString();

    QMenu *menu = interfaceMenu(window);
    ASSERT_NE(menu, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
    QWidget *menuFontRow = fontRow(menu, QStringLiteral("Menu Font Size:"));
    ASSERT_NE(menuFontRow, nullptr);
    QToolButton *plus = nullptr;
    for (QToolButton *button : menuFontRow->findChildren<QToolButton *>()) {
        if (button->text() == QStringLiteral("+")) {
            plus = button;
            break;
        }
    }
    ASSERT_NE(plus, nullptr);
    plus->click();
    QApplication::processEvents();

    for (QToolButton *button : titleMenuButtons)
        EXPECT_EQ(button->font().pointSize(), 11) << button->text().toStdString();
    for (QAbstractButton *button : fnButtons)
        EXPECT_EQ(button->font().pointSize(), 11) << button->text().toStdString();
}

} // namespace
