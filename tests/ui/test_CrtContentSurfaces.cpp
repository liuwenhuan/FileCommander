#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QPainter>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>

#include "CommandBar.h"
#include "FileListView.h"
#include "IconFileView.h"
#include "QuickView.h"
#include "ThumbnailDelegate.h"
#include "config/Settings.h"

namespace {

class StyleSheetRestore {
public:
    StyleSheetRestore() : value(qApp->styleSheet()) {}
    ~StyleSheetRestore() { qApp->setStyleSheet(value); }

private:
    QString value;
};

void applyTheme(const QString &name) {
    QFile file(QStringLiteral(TTC_SOURCE_DIR "/resources/themes/") + name +
               QStringLiteral(".qss"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    qApp->processEvents();
}

bool containsTileColours(const QImage &image, const QRect &logicalRect,
                         const QList<QColor> &colours, int tolerance = 2) {
    if (image.isNull() || colours.isEmpty())
        return false;

    const qreal dpr = image.devicePixelRatio();
    const QRect rect(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr),
                     qRound(logicalRect.width() * dpr),
                     qRound(logicalRect.height() * dpr));
    for (const QColor &expected : colours) {
        int matches = 0;
        for (int y = qMax(0, rect.top()); y <= qMin(image.height() - 1, rect.bottom()); ++y) {
            for (int x = qMax(0, rect.left()); x <= qMin(image.width() - 1, rect.right()); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (qAbs(pixel.red() - expected.red()) <= tolerance &&
                    qAbs(pixel.green() - expected.green()) <= tolerance &&
                    qAbs(pixel.blue() - expected.blue()) <= tolerance)
                    ++matches;
            }
        }
        if (matches < qMax(4, rect.width()))
            return false;
    }
    return true;
}

bool containsColourNear(const QImage &image, const QRect &logicalRect,
                        const QColor &expected, int tolerance = 12) {
    const qreal dpr = image.devicePixelRatio();
    const QRect rect(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr),
                     qRound(logicalRect.width() * dpr),
                     qRound(logicalRect.height() * dpr));
    for (int y = qMax(0, rect.top()); y <= qMin(image.height() - 1, rect.bottom()); ++y) {
        for (int x = qMax(0, rect.left()); x <= qMin(image.width() - 1, rect.right()); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (qAbs(pixel.red() - expected.red()) <= tolerance &&
                qAbs(pixel.green() - expected.green()) <= tolerance &&
                qAbs(pixel.blue() - expected.blue()) <= tolerance)
                return true;
        }
    }
    return false;
}

int changedPixelCount(const QImage &before, const QImage &after, const QRect &logicalRect,
                      int tolerance = 2) {
    const qreal dpr = after.devicePixelRatio();
    const QRect rect(qRound(logicalRect.x() * dpr), qRound(logicalRect.y() * dpr),
                     qRound(logicalRect.width() * dpr),
                     qRound(logicalRect.height() * dpr));
    int changed = 0;
    for (int y = qMax(0, rect.top()); y <= qMin(after.height() - 1, rect.bottom()); ++y) {
        for (int x = qMax(0, rect.left()); x <= qMin(after.width() - 1, rect.right()); ++x) {
            const QColor a = before.pixelColor(x, y);
            const QColor b = after.pixelColor(x, y);
            if (qAbs(a.red() - b.red()) > tolerance ||
                qAbs(a.green() - b.green()) > tolerance ||
                qAbs(a.blue() - b.blue()) > tolerance)
                ++changed;
        }
    }
    return changed;
}

const QList<QColor> kScreenTile = {
    QColor(QStringLiteral("#040b05")), QColor(QStringLiteral("#071108")),
    QColor(QStringLiteral("#060e07"))};
const QList<QColor> kSunkenTile = {
    QColor(QStringLiteral("#020c06")), QColor(QStringLiteral("#04140a")),
    QColor(QStringLiteral("#031108"))};
const QList<QColor> kChromeTile = {
    QColor(QStringLiteral("#061008")), QColor(QStringLiteral("#0a1a0d")),
    QColor(QStringLiteral("#08160b"))};

} // namespace

TEST(CrtContentSurfacesTest, QuickViewDefaultPageShowsContinuousScreenScanlines) {
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));

    QuickView view(settings);
    view.resize(420, 260);
    view.show();
    qApp->processEvents();

    EXPECT_TRUE(containsTileColours(view.grab().toImage(), QRect(12, 12, 100, 100),
                                    kScreenTile));
}

TEST(CrtContentSurfacesTest, DetailAndIconViewportsShowSunkenScanlines) {
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    FileListView details;
    details.resize(420, 260);
    details.show();
    IconFileView icons;
    icons.resize(420, 260);
    icons.show();
    qApp->processEvents();

    const QImage detailsImage = details.grab().toImage();
    const QImage iconsImage = icons.grab().toImage();
    EXPECT_TRUE(containsTileColours(detailsImage,
                                    QRect(details.viewport()->geometry().topLeft() + QPoint(20, 20),
                                          QSize(120, 120)), kSunkenTile));
    EXPECT_TRUE(containsTileColours(iconsImage,
                                    QRect(icons.viewport()->geometry().topLeft() + QPoint(20, 20),
                                          QSize(120, 120)), kSunkenTile));
}

TEST(CrtContentSurfacesTest, CommandInputShowsParentScanlinesAndKeepsSelectionReadable) {
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    CommandBar bar;
    bar.setDirectory(QStringLiteral("C:/work"));
    bar.resize(720, 42);
    bar.show();
    qApp->processEvents();
    auto *input = bar.findChild<QLineEdit *>();
    ASSERT_NE(input, nullptr);

    const QImage barImage = bar.grab().toImage();
    EXPECT_TRUE(containsTileColours(barImage,
                                    QRect(input->geometry().x() + input->width() / 2,
                                          input->geometry().y() + 3,
                                          qMax(1, input->width() / 2 - 8),
                                          qMax(1, input->height() - 6)),
                                    kChromeTile));
    const qreal dpr = barImage.devicePixelRatio();
    const int parentX = qRound((input->geometry().left() - 2) * dpr);
    const int inputX = qRound((input->geometry().center().x()) * dpr);
    int matchingRows = 0;
    int sampledRows = 0;
    for (int y = input->geometry().top() + 3; y <= input->geometry().bottom() - 3; ++y) {
        const int deviceY = qRound(y * dpr);
        if (!barImage.rect().contains(parentX, deviceY) ||
            !barImage.rect().contains(inputX, deviceY))
            continue;
        ++sampledRows;
        if (barImage.pixelColor(parentX, deviceY) == barImage.pixelColor(inputX, deviceY))
            ++matchingRows;
    }
    ASSERT_GT(sampledRows, 0);
    EXPECT_GE(matchingRows, sampledRows * 8 / 10);
    EXPECT_EQ(input->palette().color(QPalette::Highlight), QColor(0x33, 0xff, 0x88));
    EXPECT_EQ(input->palette().color(QPalette::HighlightedText), QColor(0x04, 0x14, 0x0a));
    EXPECT_EQ(input->palette().color(QPalette::Text), QColor(0x33, 0xff, 0x88));
}

TEST(CrtContentSurfacesTest, SelectedRowsRemainOpaqueAndHighContrast) {
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    QStandardItemModel model(1, 1);
    model.setData(model.index(0, 0), QStringLiteral("selected item"));
    FileListView details;
    details.setModel(&model);
    details.resize(420, 180);
    details.show();
    qApp->processEvents();
    const QRect normalVisual = details.visualRect(model.index(0, 0));
    ASSERT_FALSE(normalVisual.isEmpty());
    const QImage normalImage = details.grab().toImage();
    const QRect normalSample(details.viewport()->geometry().topLeft() +
                                 QPoint(qMax(normalVisual.left(), normalVisual.right() - 24),
                                        normalVisual.top() + 2),
                             QSize(20, qMax(1, normalVisual.height() - 4)));
    EXPECT_TRUE(containsTileColours(normalImage, normalSample, kSunkenTile));

    details.selectRow(0);
    qApp->processEvents();

    const QImage row = details.viewport()->grab().toImage();
    const QRect visual = details.visualRect(model.index(0, 0));
    ASSERT_FALSE(visual.isEmpty());
    EXPECT_EQ(row.pixelColor(qMax(visual.left(), 0) + 2, visual.center().y()),
              QColor(0x33, 0xff, 0x88));
    EXPECT_EQ(details.palette().color(QPalette::HighlightedText), QColor(0x04, 0x14, 0x0a));
}

TEST(CrtContentSurfacesTest, ThumbnailSelectionAndHoverRemainVisibleOverScanlines) {
    StyleSheetRestore restore;
    applyTheme(QStringLiteral("green"));

    QStandardItemModel model(2, 1);
    model.setData(model.index(0, 0), QStringLiteral("selected thumbnail"));
    model.setData(model.index(1, 0), QStringLiteral("hovered thumbnail"));

    IconFileView icons;
    icons.setModel(&model);
    icons.setViewMode(QListView::IconMode);
    icons.setMouseTracking(true);
    auto *delegate = new ThumbnailDelegate(&icons);
    delegate->setView(&icons);
    delegate->setIconSize(64);
    icons.setItemDelegate(delegate);
    icons.setGridSize(delegate->cellSizeHint(icons.font()));
    icons.resize(420, 260);
    icons.show();
    qApp->processEvents();

    const QModelIndex selectedIndex = model.index(0, 0);
    const QModelIndex hoveredIndex = model.index(1, 0);
    icons.selectionModel()->select(selectedIndex, QItemSelectionModel::ClearAndSelect);
    const QRect hoverRect = icons.visualRect(hoveredIndex);
    ASSERT_FALSE(hoverRect.isEmpty());
    const qreal dpr = icons.devicePixelRatioF();
    const QSize imageSize(qRound(icons.viewport()->width() * dpr),
                          qRound(icons.viewport()->height() * dpr));
    QImage beforeHover(imageSize, QImage::Format_ARGB32_Premultiplied);
    beforeHover.setDevicePixelRatio(dpr);
    beforeHover.fill(Qt::transparent);
    QImage afterHover = beforeHover.copy();
    afterHover.setDevicePixelRatio(dpr);

    QStyleOptionViewItem hoverOption;
    hoverOption.rect = hoverRect;
    hoverOption.state = QStyle::State_Enabled;
    hoverOption.palette = icons.palette();
    hoverOption.font = icons.font();
    hoverOption.fontMetrics = icons.fontMetrics();
    hoverOption.widget = &icons;
    {
        QPainter painter(&beforeHover);
        delegate->paint(&painter, hoverOption, hoveredIndex);
    }
    hoverOption.state |= QStyle::State_MouseOver;
    {
        QPainter painter(&afterHover);
        delegate->paint(&painter, hoverOption, hoveredIndex);
    }

    const QRect selectedRect = icons.visualRect(selectedIndex);
    ASSERT_FALSE(selectedRect.isEmpty());
    const QImage selectedImage = icons.viewport()->grab().toImage();
    EXPECT_TRUE(containsColourNear(selectedImage, selectedRect.adjusted(2, 2, -2, -2),
                                   QColor(0x33, 0xff, 0x88)));
    EXPECT_GT(changedPixelCount(beforeHover, afterHover, hoverRect.adjusted(4, 4, -4, -4)),
              40);
}

TEST(CrtContentSurfacesTest, LightAndDarkSurfacesDoNotAcquireCrtTiles) {
    StyleSheetRestore restore;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Settings settings(dir.filePath(QStringLiteral("settings.ini")));
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        applyTheme(theme);

        QStandardItemModel model(1, 1);
        model.setData(model.index(0, 0), QStringLiteral("ordinary row"));
        FileListView details;
        details.setModel(&model);
        details.resize(360, 220);
        details.show();
        IconFileView icons;
        icons.resize(360, 220);
        icons.show();
        CommandBar bar;
        bar.resize(620, 42);
        bar.show();
        QuickView quickView(settings);
        quickView.resize(360, 220);
        quickView.show();
        qApp->processEvents();
        auto *input = bar.findChild<QLineEdit *>();
        ASSERT_NE(input, nullptr);

        EXPECT_FALSE(containsTileColours(details.viewport()->grab().toImage(),
                                         QRect(20, 20, 100, 100), kSunkenTile))
            << theme.toStdString();
        EXPECT_FALSE(containsTileColours(input->grab().toImage(),
                                         QRect(input->width() / 2, 3,
                                               qMax(1, input->width() / 2 - 8),
                                               qMax(1, input->height() - 6)),
                                         kChromeTile))
            << theme.toStdString();
        EXPECT_FALSE(containsTileColours(icons.grab().toImage(),
                                         QRect(20, 20, 100, 100), kSunkenTile))
            << theme.toStdString();
        EXPECT_FALSE(containsTileColours(quickView.grab().toImage(),
                                         QRect(12, 12, 100, 100), kScreenTile))
            << theme.toStdString();

        const QRect visual = details.visualRect(model.index(0, 0));
        ASSERT_FALSE(visual.isEmpty());
        const QImage detailsImage = details.grab().toImage();
        const QRect ordinaryRow(details.viewport()->geometry().topLeft() +
                                    visual.topLeft(),
                                visual.size());
        EXPECT_TRUE(containsColourNear(detailsImage, ordinaryRow.adjusted(5, 2, -5, -2),
                                       details.palette().color(QPalette::Base), 2))
            << theme.toStdString();
    }
}
