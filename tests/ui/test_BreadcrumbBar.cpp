#include <gtest/gtest.h>


#include <QApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTest>
#include <QToolButton>

#include "BreadcrumbBar.h"

namespace {

QStringList linkTargets(const BreadcrumbBar &bar) {
    const auto *label = bar.findChild<QLabel *>();
    if (!label)
        return {};

    QStringList targets;
    QRegularExpression links(QStringLiteral("href=\"([^\"]+)\""));
    auto match = links.globalMatch(label->text());
    while (match.hasNext())
        targets.append(match.next().captured(1));
    return targets;
}

void applyThemeSheet(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name +
               QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

bool edgeIsMostlyDifferentFromInner(const QImage &image, Qt::Edge edge,
                                    int tolerance = 8) {
    const int length = (edge == Qt::TopEdge || edge == Qt::BottomEdge)
                           ? image.width()
                           : image.height();
    const int fixed = (edge == Qt::BottomEdge) ? image.height() - 1
                     : (edge == Qt::RightEdge) ? image.width() - 1
                                               : 0;
    int different = 0;
    int samples = 0;
    for (int offset = 2; offset < length - 2; ++offset) {
        const bool bottomOrRight = edge == Qt::BottomEdge || edge == Qt::RightEdge;
        const int inner = bottomOrRight ? fixed - 1 : fixed + 1;
        const int x = (edge == Qt::TopEdge || edge == Qt::BottomEdge) ? offset : fixed;
        const int y = (edge == Qt::LeftEdge || edge == Qt::RightEdge) ? offset : fixed;
        const int innerX = (edge == Qt::TopEdge || edge == Qt::BottomEdge) ? offset : inner;
        const int innerY = (edge == Qt::LeftEdge || edge == Qt::RightEdge) ? offset : inner;
        const QColor outerPixel = image.pixelColor(x, y);
        const QColor innerPixel = image.pixelColor(innerX, innerY);
        ++samples;
        if (qAbs(outerPixel.red() - innerPixel.red()) > tolerance ||
            qAbs(outerPixel.green() - innerPixel.green()) > tolerance ||
            qAbs(outerPixel.blue() - innerPixel.blue()) > tolerance)
            ++different;
    }
    return samples > 0 && different * 100 >= samples * 80;
}

bool columnIsMostlyDifferentFromNeighbors(const QImage &image, int column,
                                          int tolerance = 8) {
    if (column <= 0 || column >= image.width() - 1)
        return false;

    int different = 0;
    int samples = 0;
    for (int row = 2; row < image.height() - 2; ++row) {
        const QColor pixel = image.pixelColor(column, row);
        const QColor left = image.pixelColor(column - 1, row);
        const QColor right = image.pixelColor(column + 1, row);
        ++samples;
        const auto differs = [&](const QColor &neighbor) {
            return qAbs(pixel.red() - neighbor.red()) > tolerance ||
                   qAbs(pixel.green() - neighbor.green()) > tolerance ||
                   qAbs(pixel.blue() - neighbor.blue()) > tolerance;
        };
        if (differs(left) && differs(right))
            ++different;
    }
    return samples > 0 && different * 100 >= samples * 80;
}

struct RestoreStyleSheet {
    QString value;
    ~RestoreStyleSheet() { qApp->setStyleSheet(value); }
};

} // namespace

TEST(BreadcrumbBar, WindowsDriveSegmentsKeepDriveRootInEveryTarget) {
    BreadcrumbBar bar;

    bar.setPath(QStringLiteral("C:/Users/deepin/Documents"));

    EXPECT_EQ(linkTargets(bar),
              QStringList({QStringLiteral("C:/"),
                           QStringLiteral("C:/Users"),
                           QStringLiteral("C:/Users/deepin"),
                           QStringLiteral("C:/Users/deepin/Documents")}));
}

TEST(BreadcrumbBar, WindowsDriveRootIsAValidNavigationTarget) {
    BreadcrumbBar bar;

    bar.setPath(QStringLiteral("C:/"));

    EXPECT_EQ(linkTargets(bar), QStringList({QStringLiteral("C:/")}));
}

TEST(BreadcrumbBar, UnixSegmentsRemainAbsolute) {
    BreadcrumbBar bar;

    bar.setPath(QStringLiteral("/home/deepin/Documents"));

    EXPECT_EQ(linkTargets(bar),
              QStringList({QStringLiteral("/"),
                           QStringLiteral("/home"),
                           QStringLiteral("/home/deepin"),
                           QStringLiteral("/home/deepin/Documents")}));
}

TEST(BreadcrumbBar, AddressRowDrawsOnlyItsTwoOuterVerticalBorders) {
    RestoreStyleSheet restore{qApp->styleSheet()};
    applyThemeSheet(QStringLiteral("light"));

    QWidget panel;
    auto *row = new QWidget(&panel);
    row->setObjectName(QStringLiteral("PanelAddressRow"));
    row->setStyleSheet(QStringLiteral(
        "#PanelAddressRow { background: #24313b; border: none; }"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *tree = new QToolButton(row);
    tree->setObjectName(QStringLiteral("PanelTreeButton"));
    auto *back = new QToolButton(row);
    auto *forward = new QToolButton(row);
    auto *bar = new BreadcrumbBar(&panel);
    auto *star = new QToolButton(row);
    for (QToolButton *button : {tree, back, forward, star})
        button->setFixedSize(32, 30);
    bar->setPath(QStringLiteral("C:/Users/deepin/Documents"));
    bar->setFixedHeight(30);
    layout->addWidget(tree);
    layout->addWidget(back);
    layout->addWidget(forward);
    layout->addWidget(bar, 1);
    layout->addWidget(star);
    row->setGeometry(0, 0, 800, 30);
    panel.resize(800, 30);
    panel.show();
    qApp->processEvents();

    const QImage image = row->grab().toImage();
    EXPECT_TRUE(edgeIsMostlyDifferentFromInner(image, Qt::LeftEdge));
    EXPECT_TRUE(edgeIsMostlyDifferentFromInner(image, Qt::RightEdge));
    EXPECT_FALSE(edgeIsMostlyDifferentFromInner(image, Qt::TopEdge));
    EXPECT_FALSE(edgeIsMostlyDifferentFromInner(image, Qt::BottomEdge));

    for (int item = 0; item + 1 < layout->count(); ++item) {
        const QWidget *left = layout->itemAt(item)->widget();
        const QWidget *right = layout->itemAt(item + 1)->widget();
        ASSERT_NE(left, nullptr);
        ASSERT_NE(right, nullptr);
        const int gap = (left->geometry().right() + right->geometry().left()) / 2;
        EXPECT_FALSE(columnIsMostlyDifferentFromNeighbors(image, gap));
    }
}

TEST(BreadcrumbBar, OverflowArrowsExposeDirectionAndReachBothEnds) {
    RestoreStyleSheet restore{qApp->styleSheet()};
    applyThemeSheet(QStringLiteral("light"));
    BreadcrumbBar bar;
    bar.resize(280, 30);
    bar.setPath(QStringLiteral(
        "C:/Users/deepin/AppData/Local/Temp/FileCommander-package-smoke-"
        "cc253534-8737-41be-8024-fecb73e6ef11/one/two/three/four"));
    bar.show();
    qApp->processEvents();

    QToolButton *left =
        bar.findChild<QToolButton *>(QStringLiteral("BreadcrumbScrollLeft"));
    QToolButton *right =
        bar.findChild<QToolButton *>(QStringLiteral("BreadcrumbScrollRight"));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(left->isVisible());
    ASSERT_TRUE(right->isVisible());
    EXPECT_FALSE(left->isEnabled());
    EXPECT_TRUE(right->isEnabled());
    EXPECT_LT(left->geometry().right(), right->geometry().left());
    QWidget *viewport = bar.findChild<QWidget *>(QStringLiteral("BreadcrumbViewport"));
    QLabel *label = bar.findChild<QLabel *>();
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(label, nullptr);
    EXPECT_LT(left->geometry().right(), viewport->geometry().left());
    EXPECT_LT(viewport->geometry().right(), right->geometry().left());
    EXPECT_GT(viewport->width(), 0);
    EXPECT_TRUE(label->isVisible());

    for (int click = 0; click < 100 && right->isEnabled(); ++click)
        QTest::mouseClick(right, Qt::LeftButton);
    qApp->processEvents();
    EXPECT_TRUE(left->isEnabled());
    EXPECT_FALSE(right->isEnabled());

    for (int click = 0; click < 100 && left->isEnabled(); ++click)
        QTest::mouseClick(left, Qt::LeftButton);
    qApp->processEvents();
    EXPECT_FALSE(left->isEnabled());
    EXPECT_TRUE(right->isEnabled());
}

TEST(BreadcrumbBar, OverflowStateTracksResizeNavigationThemeAndEditing) {
    RestoreStyleSheet restore{qApp->styleSheet()};
    applyThemeSheet(QStringLiteral("dark"));
    BreadcrumbBar bar;
    bar.resize(240, 30);
    const QString longPath = QStringLiteral(
        "C:/Users/deepin/Documents/filecommander/build/windows/full/release/output");
    bar.setPath(longPath);
    bar.show();
    qApp->processEvents();

    QToolButton *left =
        bar.findChild<QToolButton *>(QStringLiteral("BreadcrumbScrollLeft"));
    QToolButton *right =
        bar.findChild<QToolButton *>(QStringLiteral("BreadcrumbScrollRight"));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(right->isVisible());
    QTest::mouseClick(right, Qt::LeftButton);
    ASSERT_TRUE(left->isEnabled());

    bar.setPath(longPath + QStringLiteral("/next"));
    qApp->processEvents();
    EXPECT_FALSE(left->isEnabled());
    EXPECT_TRUE(right->isEnabled());

    bar.resize(3000, 30);
    qApp->processEvents();
    EXPECT_FALSE(left->isVisible());
    EXPECT_FALSE(right->isVisible());

    bar.resize(240, 30);
    qApp->processEvents();
    ASSERT_TRUE(right->isVisible());
    applyThemeSheet(QStringLiteral("green"));
    EXPECT_FALSE(left->isEnabled());
    EXPECT_TRUE(right->isEnabled());

    bar.setPath(QStringLiteral("C:/Temp"));
    qApp->processEvents();
    EXPECT_FALSE(left->isVisible());
    EXPECT_FALSE(right->isVisible());
    bar.setPath(longPath + QStringLiteral("/next"));
    qApp->processEvents();
    ASSERT_TRUE(right->isVisible());

    QWidget *segments = bar.findChild<QWidget *>(QStringLiteral("BreadcrumbSegments"));
    ASSERT_NE(segments, nullptr);
    QTest::mouseDClick(segments, Qt::LeftButton, Qt::NoModifier,
                      segments->rect().center());
    qApp->processEvents();
    QLineEdit *editor = bar.findChild<QLineEdit *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->isVisible());
    EXPECT_EQ(editor->text(), longPath + QStringLiteral("/next"));
    EXPECT_FALSE(left->isVisible());
    EXPECT_FALSE(right->isVisible());
}
