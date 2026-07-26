#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QPainter>
#include <QSizeF>
#include <QVector>

#include "SlideSceneBuilder.h"

// office_oxide inlines a full base64 copy of a master/layout picture into every
// slide that inherits it, so a deck's background can arrive 18 times over. These
// tests pin the two properties that keep that from costing 18 decodes: identical
// payloads share one decoded QPixmap, and an absurdly oversized picture is decoded
// downscaled instead of at full resolution.
namespace {

// Matches the root element office_oxide emits -- the xlink namespace declaration
// matters, since that is how the <image> href is resolved.
QByteArray wrapSvg(const QByteArray &body) {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
           "viewBox=\"0 0 12192000 6858000\">" +
           body + "</svg>";
}

// A PNG payload with enough entropy that it can't be trivially collapsed, so a
// decode is real work and a downscale is observable.
QByteArray makePngBase64(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    for (int i = 0; i < 40; ++i) {
        p.setBrush(QColor((i * 37) % 256, (i * 91) % 256, (i * 53) % 256));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPoint((i * 197) % w, (i * 131) % h), w / 20 + 1, h / 20 + 1);
    }
    p.end();

    QByteArray raw;
    QBuffer buf(&raw);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return raw.toBase64();
}

QByteArray imageTag(const QByteArray &b64, const char *extraAttrs = "") {
    return QByteArray("<image x=\"0\" y=\"0\" width=\"2853840\" height=\"540000\" ") + extraAttrs +
           " xlink:href=\"data:image/png;base64," + b64 + "\"/>";
}

// A page-sized <image>, as office_oxide emits for a <p:bg> picture background:
// the box is the whole 16:9 slide, so a non-16:9 source makes the stretch-vs-fit
// difference plainly measurable.
QByteArray pageImageTag(const QByteArray &b64, const char *extraAttrs = "") {
    return QByteArray("<image x=\"0\" y=\"0\" width=\"12192000\" height=\"6858000\" ") +
           extraAttrs + " xlink:href=\"data:image/png;base64," + b64 + "\"/>";
}

void collectPixmapItems(QGraphicsItem *item, QVector<QGraphicsPixmapItem *> *out) {
    if (auto *p = qgraphicsitem_cast<QGraphicsPixmapItem *>(item))
        out->push_back(p);
    for (QGraphicsItem *child : item->childItems())
        collectPixmapItems(child, out);
}

QVector<QGraphicsPixmapItem *> pixmapsOf(QGraphicsItem *page) {
    QVector<QGraphicsPixmapItem *> out;
    if (page)
        collectPixmapItems(page, &out);
    return out;
}

} // namespace

// The same picture inlined into two different slides must decode once: both items
// end up holding the very same QPixmap (equal cacheKey), not two equal-looking
// copies. This is what turns an 18x repeated background into a single decode.
TEST(SlideSceneImageCache, RepeatedPictureDecodesOnceAndIsShared) {
    const QByteArray b64 = makePngBase64(600, 400);

    QGraphicsItem *pageA = SlideScene::buildSlidePage(wrapSvg(imageTag(b64)), nullptr, nullptr);
    QGraphicsItem *pageB = SlideScene::buildSlidePage(wrapSvg(imageTag(b64)), nullptr, nullptr);
    ASSERT_NE(pageA, nullptr);
    ASSERT_NE(pageB, nullptr);

    const QVector<QGraphicsPixmapItem *> a = pixmapsOf(pageA);
    const QVector<QGraphicsPixmapItem *> b = pixmapsOf(pageB);
    ASSERT_EQ(a.size(), 1);
    ASSERT_EQ(b.size(), 1);
    EXPECT_FALSE(a[0]->pixmap().isNull());
    EXPECT_EQ(a[0]->pixmap().cacheKey(), b[0]->pixmap().cacheKey())
        << "the second slide re-decoded an identical payload instead of reusing the cached pixmap";

    delete pageA;
    delete pageB;
}

// Two *different* pictures must not collide in the cache.
TEST(SlideSceneImageCache, DifferentPicturesAreNotConflated) {
    QGraphicsItem *pageA =
        SlideScene::buildSlidePage(wrapSvg(imageTag(makePngBase64(600, 400))), nullptr, nullptr);
    QGraphicsItem *pageB =
        SlideScene::buildSlidePage(wrapSvg(imageTag(makePngBase64(320, 240))), nullptr, nullptr);
    ASSERT_NE(pageA, nullptr);
    ASSERT_NE(pageB, nullptr);

    const QVector<QGraphicsPixmapItem *> a = pixmapsOf(pageA);
    const QVector<QGraphicsPixmapItem *> b = pixmapsOf(pageB);
    ASSERT_EQ(a.size(), 1);
    ASSERT_EQ(b.size(), 1);
    EXPECT_NE(a[0]->pixmap().cacheKey(), b[0]->pixmap().cacheKey());
    EXPECT_EQ(a[0]->pixmap().size(), QSize(600, 400));
    EXPECT_EQ(b[0]->pixmap().size(), QSize(320, 240));

    delete pageA;
    delete pageB;
}

// A picture within the pixel budget is decoded untouched -- no silent quality loss
// for ordinary photos.
TEST(SlideSceneImageCache, ModeratePictureKeepsFullResolution) {
    QGraphicsItem *page =
        SlideScene::buildSlidePage(wrapSvg(imageTag(makePngBase64(1500, 1000))), nullptr, nullptr);
    ASSERT_NE(page, nullptr);
    const QVector<QGraphicsPixmapItem *> items = pixmapsOf(page);
    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items[0]->pixmap().size(), QSize(1500, 1000));
    delete page;
}

// A picture far past the pixel budget (real decks embed 76 Mpx assets into strips
// a few hundred pixels wide) is read back downscaled, preserving aspect ratio.
// Full resolution would cost hundreds of MB of bitmap for no visible detail.
TEST(SlideSceneImageCache, OversizedPictureIsDecodedDownscaled) {
    const QByteArray b64 = makePngBase64(9000, 3000); // 27 Mpx, over the 16 Mpx budget
    QGraphicsItem *page = SlideScene::buildSlidePage(wrapSvg(imageTag(b64)), nullptr, nullptr);
    ASSERT_NE(page, nullptr);

    const QVector<QGraphicsPixmapItem *> items = pixmapsOf(page);
    ASSERT_EQ(items.size(), 1);
    const QSize sz = items[0]->pixmap().size();
    EXPECT_LE(qMax(sz.width(), sz.height()), 4096);
    EXPECT_GT(sz.width(), 0);
    // 9000x3000 is 3:1; the downscale must not distort it.
    EXPECT_NEAR(double(sz.width()) / double(sz.height()), 3.0, 0.05);

    delete page;
}

// Cropping (a:srcRect -> data-crop-*) still cuts the right sub-rect out of a
// cached picture, and must not corrupt the cached original that other slides
// share. 25% off the left and 50% off the bottom leaves a 75% x 50% sub-rect.
TEST(SlideSceneImageCache, CropUsesCachedSourceWithoutMutatingIt) {
    const QByteArray b64 = makePngBase64(800, 400);

    QGraphicsItem *cropped = SlideScene::buildSlidePage(
        wrapSvg(imageTag(b64, "data-crop-l=\"25000\" data-crop-b=\"50000\"")), nullptr, nullptr);
    ASSERT_NE(cropped, nullptr);
    const QVector<QGraphicsPixmapItem *> c = pixmapsOf(cropped);
    ASSERT_EQ(c.size(), 1);
    EXPECT_EQ(c[0]->pixmap().size(), QSize(600, 200));

    // The uncropped picture built afterwards must still be the full image: the
    // crop copied out of the cached pixmap rather than shrinking it in place.
    QGraphicsItem *full = SlideScene::buildSlidePage(wrapSvg(imageTag(b64)), nullptr, nullptr);
    ASSERT_NE(full, nullptr);
    const QVector<QGraphicsPixmapItem *> f = pixmapsOf(full);
    ASSERT_EQ(f.size(), 1);
    EXPECT_EQ(f[0]->pixmap().size(), QSize(800, 400));

    delete cropped;
    delete full;
}

// A <p:bg> picture background carries data-stretch="1" (PowerPoint's
// <a:stretch><a:fillRect/>), which means "cover the page", not "fit inside it".
// A 3:2 source on a 16:9 slide must be scaled non-uniformly to the full box --
// letterboxing it would leave white bars down both sides that PowerPoint does
// not show.
TEST(SlideSceneImageCache, StretchedBackgroundFillsTheWholeBox) {
    const QByteArray b64 = makePngBase64(600, 400); // 3:2, unlike the 16:9 page
    QGraphicsItem *page = SlideScene::buildSlidePage(
        wrapSvg(pageImageTag(b64, "data-stretch=\"1\"")), nullptr, nullptr);
    ASSERT_NE(page, nullptr);

    const QVector<QGraphicsPixmapItem *> items = pixmapsOf(page);
    ASSERT_EQ(items.size(), 1);
    // 12192000 x 6858000 EMU at kSceneScale (0.01) == 121920 x 68580 scene units.
    const QRectF box = items[0]->sceneBoundingRect();
    EXPECT_NEAR(box.width(), 121920.0, 1.0);
    EXPECT_NEAR(box.height(), 68580.0, 1.0);

    delete page;
}

// The same picture in the same box *without* the marker keeps the historical
// uniform xMidYMid-meet fit: 600x400 into 121920x68580 scales by min(203.2,
// 171.45) = 171.45, so the height fills and the width falls short. This is the
// old-oxide / new-FileCommander compatibility case -- no attribute, no change.
TEST(SlideSceneImageCache, UnstretchedImageKeepsUniformFit) {
    const QByteArray b64 = makePngBase64(600, 400);
    QGraphicsItem *page =
        SlideScene::buildSlidePage(wrapSvg(pageImageTag(b64)), nullptr, nullptr);
    ASSERT_NE(page, nullptr);

    const QVector<QGraphicsPixmapItem *> items = pixmapsOf(page);
    ASSERT_EQ(items.size(), 1);
    const QRectF box = items[0]->sceneBoundingRect();
    EXPECT_NEAR(box.height(), 68580.0, 1.0);
    EXPECT_NEAR(box.width(), 102870.0, 1.0) << "uniform fit must letterbox, not stretch";
    // Aspect ratio of the source survives.
    EXPECT_NEAR(box.width() / box.height(), 1.5, 0.01);

    delete page;
}

// data-stretch="0" (and any other falsey value) is not a stretch request, so a
// future emitter that spells the default out explicitly still gets the uniform
// fit rather than silently flipping every picture to fill.
TEST(SlideSceneImageCache, StretchZeroIsNotAStretch) {
    const QByteArray b64 = makePngBase64(600, 400);
    QGraphicsItem *page = SlideScene::buildSlidePage(
        wrapSvg(pageImageTag(b64, "data-stretch=\"0\"")), nullptr, nullptr);
    ASSERT_NE(page, nullptr);

    const QVector<QGraphicsPixmapItem *> items = pixmapsOf(page);
    ASSERT_EQ(items.size(), 1);
    EXPECT_NEAR(items[0]->sceneBoundingRect().width(), 102870.0, 1.0);

    delete page;
}

// A stretched background that also had to be decoded downscaled still covers the
// page: sx/sy divide by the *decoded* pixel size, so the downsample is absorbed.
// The real 0511.pptx backdrop takes this path.
TEST(SlideSceneImageCache, OversizedStretchedBackgroundStillCoversThePage) {
    const QByteArray b64 = makePngBase64(9000, 3000); // 27 Mpx -> decoded downscaled
    QGraphicsItem *page = SlideScene::buildSlidePage(
        wrapSvg(pageImageTag(b64, "data-stretch=\"1\"")), nullptr, nullptr);
    ASSERT_NE(page, nullptr);

    const QVector<QGraphicsPixmapItem *> items = pixmapsOf(page);
    ASSERT_EQ(items.size(), 1);
    EXPECT_LE(qMax(items[0]->pixmap().width(), items[0]->pixmap().height()), 4096)
        << "precondition: this payload must actually be downsampled";
    const QRectF box = items[0]->sceneBoundingRect();
    EXPECT_NEAR(box.width(), 121920.0, 1.0);
    EXPECT_NEAR(box.height(), 68580.0, 1.0);

    delete page;
}
