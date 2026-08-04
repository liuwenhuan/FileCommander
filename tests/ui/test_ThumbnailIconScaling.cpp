#include <gtest/gtest.h>

#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>

#include "FileSystemModel.h"
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

// The other half of the same defect, and the one actually reported: Windows'
// jumbo image list hands back a 256x256 slot for EVERY icon, including one that
// never shipped a 256px variant -- and it drops the smaller bitmap in the
// top-left corner rather than centring it. Measured for .rar (7-Zip's icon):
// a 256 canvas holding 44x34 of ink at (2,7).
//
// That is not "an icon too small to fill the box" -- it is a full-size pixmap
// that is mostly empty, so the upscale above never fires and the grid drew a
// badge pinned to a corner of a large tile. IconCache crops the padding away so
// the icon reports the size it really has, and the upscale takes it from there.
TEST(ThumbnailIconScaling, AnIconTheShellPaddedIntoAJumboSlotStillFillsTheTile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const char *name : {"volume.rar", "archive.zip", "notes.txt"}) {
        QFile file(QDir(dir.path()).filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
    }

    FileSystemModel model;
    QSignalSpy loaded(&model, &FileSystemModel::loadFinished);
    model.setRootPath(dir.path());
    if (loaded.isEmpty())
        ASSERT_TRUE(loaded.wait(5000));
    ASSERT_GT(model.rowCount(), 0);

    ThumbnailDelegate delegate;
    delegate.setIconSize(192);
    delegate.setFontPointSize(12);

    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.isParentEntry(row))
            continue;
        const QString name = model.index(row, 0).data(Qt::DisplayRole).toString();
        QStyleOptionViewItem option;
        option.rect = QRect(QPoint(0, 0), delegate.cellSizeHint(option.font));
        option.state = QStyle::State_Enabled;

        QPixmap canvas(option.rect.size());
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        delegate.paint(&painter, option, model.index(row, 0));
        painter.end();

        const QImage image = canvas.toImage();
        QRect ink;
        for (int y = 0; y < qMin(212, image.height()); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(image.pixel(x, y)) > 8)
                    ink = ink.united(QRect(x, y, 1, 1));
        ASSERT_FALSE(ink.isEmpty()) << name.toStdString();
        // Not the full 192: icons carry their own padding and some are far from
        // square. But a badge marooned in a corner measured 34px, so anything
        // near the box rules that out.
        EXPECT_GE(qMax(ink.width(), ink.height()), 150)
            << name.toStdString() << " drew " << ink.width() << "x" << ink.height()
            << " of ink in a 192px box";
    }
}
