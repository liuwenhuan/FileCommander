#include <gtest/gtest.h>

#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>

#include "ThumbnailDelegate.h"

// QIcon::pixmap() scales a bitmap DOWN to the requested size but never up: ask
// a 32px icon for 192px and you get 32px back. Most file types have a 256px
// variant registered, so most icons fill the thumbnail box -- but the ones that
// do not were drawn at their native size, a small badge marooned in a large
// tile while the folders beside them filled theirs.
//
// A grid whose whole point is a size the user chose cannot leave that to
// whether an icon's author shipped a large variant.
namespace {

// One row whose icon has a single pixmap of the given size and nothing larger.
QStandardItemModel *modelWithIcon(int width, int height, QObject *parent) {
    QPixmap pixmap(width, height);
    pixmap.fill(Qt::red);
    auto *model = new QStandardItemModel(parent);
    auto *item = new QStandardItem(QStringLiteral("small.icon"));
    item->setIcon(QIcon(pixmap));
    model->setItem(0, item);
    return model;
}

// Bounding box of everything painted in the icon half of the cell.
QRect inkOfIconArea(ThumbnailDelegate &delegate, QAbstractItemModel *model, int iconSize) {
    QStyleOptionViewItem option;
    option.rect = QRect(QPoint(0, 0), delegate.cellSizeHint(option.font));
    option.state = QStyle::State_Enabled;

    QPixmap canvas(option.rect.size());
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    delegate.paint(&painter, option, model->index(0, 0));
    painter.end();

    const QImage image = canvas.toImage();
    QRect ink;
    const int iconBottom = qMin(iconSize + 20, image.height()); // stop above the label
    for (int y = 0; y < iconBottom; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8)
                ink = ink.united(QRect(x, y, 1, 1));
        }
    }
    return ink;
}

} // namespace

TEST(ThumbnailIconScaling, AnIconWithNoLargeVariantStillFillsTheTile) {
    QObject owner;
    ThumbnailDelegate delegate;
    delegate.setIconSize(192);
    delegate.setFontPointSize(12);

    const QRect ink = inkOfIconArea(delegate, modelWithIcon(32, 32, &owner), 192);
    ASSERT_FALSE(ink.isEmpty());
    // Within a couple of pixels of the box: this icon is square, so it should
    // reach the full 192 rather than sit at its native 32.
    EXPECT_GE(ink.width(), 190)
        << "a 32px icon drew " << ink.width() << "px wide in a 192px box";
    EXPECT_GE(ink.height(), 190);
    // And no further: growing it in device pixels while the size asked for was
    // logical would overflow the box by the display's scale factor.
    EXPECT_LE(ink.width(), 192);
    EXPECT_LE(ink.height(), 192);
}

TEST(ThumbnailIconScaling, AWideIconKeepsItsShape) {
    QObject owner;
    ThumbnailDelegate delegate;
    delegate.setIconSize(192);
    delegate.setFontPointSize(12);

    // 4:1. Filling the box in both directions would square it up.
    const QRect ink = inkOfIconArea(delegate, modelWithIcon(64, 16, &owner), 192);
    ASSERT_FALSE(ink.isEmpty());
    EXPECT_GE(ink.width(), 190);
    EXPECT_LE(ink.width(), 192);
    EXPECT_NEAR(ink.height(), ink.width() / 4.0, 3.0)
        << ink.width() << "x" << ink.height() << " is not the source's 4:1";
}

TEST(ThumbnailIconScaling, AnIconThatIsAlreadyBigEnoughIsLeftAlone) {
    QObject owner;
    ThumbnailDelegate delegate;
    delegate.setIconSize(192);
    delegate.setFontPointSize(12);

    // 256 available, 192 asked for: QIcon scales it down itself, and the result
    // must not then be stretched back out by the upscale path.
    const QRect ink = inkOfIconArea(delegate, modelWithIcon(256, 256, &owner), 192);
    ASSERT_FALSE(ink.isEmpty());
    EXPECT_LE(ink.width(), 192);
    EXPECT_GE(ink.width(), 190);
}
