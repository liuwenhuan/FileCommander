#include "Phosphor.h"

namespace fc {
namespace {
QColor g_contentTint; // invalid by default: content is left alone
int g_pixelBlock = 0; // 0: no quantisation

// Fewest cells an image may be reduced to. 8 is the width of an era-appropriate
// icon and stays identifiable -- a folder is still a folder at 8x8. Below that
// the block is shrunk rather than the image being mangled, because an
// unrecognisable icon is a worse outcome than one slightly too sharp.
constexpr int kMinCells = 8;

QString phosphorMixer(const QColor &tint) {
    const qreal r = tint.redF(), g = tint.greenF(), b = tint.blueF();
    const auto n = [](qreal v) { return QString::number(v, 'f', 4); };
    return QStringLiteral("colorchannelmixer="
                          "rr=%1:rg=%2:rb=%3:"
                          "gr=%4:gg=%5:gb=%6:"
                          "br=%7:bg=%8:bb=%9")
        .arg(n(r * kLumaR), n(r * kLumaG), n(r * kLumaB))
        .arg(n(g * kLumaR), n(g * kLumaG), n(g * kLumaB))
        .arg(n(b * kLumaR), n(b * kLumaG), n(b * kLumaB));
}

QString scanlineStage(int period, qreal darken) {
    if (period < 2 || darken <= 0.0)
        return {};
    const qreal attenuation = 1.0 - qBound(0.0, darken, 1.0);
    const QString expression = QStringLiteral("if(eq(mod(y\\,%1)\\,0)\\,val*%2\\,val)")
                                   .arg(period)
                                   .arg(QString::number(attenuation, 'f', 3));
    return QStringLiteral("lutrgb=r='%1':g='%1':b='%1'").arg(expression);
}
} // namespace

QColor contentTint() {
    return g_contentTint;
}

void setContentTint(const QColor &tint) {
    g_contentTint = tint;
}

int contentPixelBlock() {
    return g_pixelBlock;
}

void setContentPixelBlock(int blockPixels) {
    g_pixelBlock = qMax(0, blockPixels);
}

void pixelate(QImage &image, int block) {
    if (block < 2 || image.isNull())
        return;
    const int shortest = qMin(image.width(), image.height());
    if (shortest < kMinCells * 2)
        return; // too small to quantise into anything legible at all
    // Shrink the block rather than refuse outright when the image is modest:
    // an icon keeps at least kMinCells cells across, larger surfaces get the
    // full block.
    const int effective = qBound(2, block, qMax(2, shortest / kMinCells));

    const QSize cells((image.width() + effective - 1) / effective,
                      (image.height() + effective - 1) / effective);
    if (cells.width() < 1 || cells.height() < 1)
        return;
    // Smooth down (each cell becomes the average of what it covers), then hard
    // back up (each cell becomes a flat square). IgnoreAspectRatio on both so
    // the round trip lands on exactly the original size.
    const QImage small =
        image.scaled(cells, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image = small.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

void tintImage(QImage &image, const QColor &tint, qreal floor) {
    if (!tint.isValid() || image.isNull())
        return;
    if (image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_ARGB32);

    // Precompute the ramp: for a source luma L in 0..255, the output channel is
    // tint * (floor + (1 - floor) * L/255). 256 entries per channel beats doing
    // the multiply per pixel, and a preview image is millions of pixels.
    quint8 rampR[256], rampG[256], rampB[256];
    for (int l = 0; l < 256; ++l) {
        const qreal k = floor + (1.0 - floor) * (l / 255.0);
        rampR[l] = static_cast<quint8>(qBound(0, qRound(tint.red() * k), 255));
        rampG[l] = static_cast<quint8>(qBound(0, qRound(tint.green() * k), 255));
        rampB[l] = static_cast<quint8>(qBound(0, qRound(tint.blue() * k), 255));
    }

    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int a = qAlpha(line[x]);
            if (a == 0)
                continue; // nothing was drawn here; tinting the void adds a haze
            const int lum = qBound(0, qRound(kLumaR * qRed(line[x]) + kLumaG * qGreen(line[x])
                                             + kLumaB * qBlue(line[x])),
                                   255);
            line[x] = qRgba(rampR[lum], rampG[lum], rampB[lum], a);
        }
    }
}

QPixmap tintedPixmap(const QPixmap &src, const QColor &tint, int blockPixels, qreal floor) {
    if (!tint.isValid() || src.isNull())
        return src;
    QImage img = src.toImage();
    // Quantise first, colourise second -- see pixelate()'s note.
    pixelate(img, blockPixels < 0 ? contentPixelBlock() : blockPixels);
    tintImage(img, tint, floor);
    QPixmap out = QPixmap::fromImage(img);
    out.setDevicePixelRatio(src.devicePixelRatio()); // neither step may resize anything
    return out;
}

QString mpvFilterFor(const QColor &tint, int blockPixels, int displayWidthPixels) {
    if (!tint.isValid())
        return {};

    QString quantise;
    if (blockPixels >= 2 && displayWidthPixels > 0) {
        // Target width in cells: how wide the picture is on screen divided by
        // the cell size. Reducing to THAT (rather than to a fraction of the
        // source) is what makes a 4K clip and a 480p clip come out with the
        // same-sized squares, since only the on-screen size is involved.
        //
        // Height is left to -2 so libavfilter keeps the aspect ratio on an even
        // number of lines (yuv420 needs that; an odd height is an error).
        const int cells = qBound(24, displayWidthPixels / blockPixels, 1920);
        quantise = QStringLiteral("scale=w=%1:h=-2:flags=neighbor,"
                                  "scale=w=%2:h=-2:flags=neighbor,")
                       .arg(cells)
                       .arg(cells * blockPixels);
    }
    // colorchannelmixer computes each output channel as a weighted sum of the
    // input channels. Setting row `c` to tint[c] * (lumaR, lumaG, lumaB) makes
    // every output channel tint[c] * luma -- i.e. exactly tintImage() with a
    // floor of 0. Nine coefficients, one filter, no chained format conversion.
    const qreal r = tint.redF(), g = tint.greenF(), b = tint.blueF();
    const auto n = [](qreal v) { return QString::number(v, 'f', 4); };
    const QString mixer = QStringLiteral("colorchannelmixer="
                                         "rr=%1:rg=%2:rb=%3:"
                                         "gr=%4:gg=%5:gb=%6:"
                                         "br=%7:bg=%8:bb=%9")
                              .arg(n(r * kLumaR), n(r * kLumaG), n(r * kLumaB))
                              .arg(n(g * kLumaR), n(g * kLumaG), n(g * kLumaB))
                              .arg(n(b * kLumaR), n(b * kLumaG), n(b * kLumaB));
    return QStringLiteral("lavfi=[%1%2]").arg(quantise, mixer);
}

void applyScanlines(QImage &image, int period, qreal darken) {
    if (image.isNull() || period < 2 || darken <= 0.0)
        return;
    if (image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_ARGB32);

    const qreal attenuation = 1.0 - qBound(0.0, darken, 1.0);
    for (int y = 0; y < image.height(); ++y) {
        if (y % period != 0)
            continue;
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            line[x] = qRgba(qRound(qRed(pixel) * attenuation),
                            qRound(qGreen(pixel) * attenuation),
                            qRound(qBlue(pixel) * attenuation), qAlpha(pixel));
        }
    }
}

QPixmap scanlinedPhosphorPixmap(const QPixmap &src, const QColor &tint, int period,
                                qreal darken) {
    if (src.isNull() || !tint.isValid())
        return src;
    QImage image = src.toImage().convertToFormat(QImage::Format_ARGB32);
    tintImage(image, tint);
    applyScanlines(image, period, darken);
    QPixmap result = QPixmap::fromImage(image);
    result.setDevicePixelRatio(src.devicePixelRatio());
    return result;
}

QString mpvScanlinedPhosphorFilter(const QColor &tint, int period, qreal darken) {
    if (!tint.isValid())
        return {};
    const QString scanlines = scanlineStage(period, darken);
    const QString chain = scanlines.isEmpty() ? phosphorMixer(tint)
                                               : phosphorMixer(tint) + QLatin1Char(',') + scanlines;
    return QStringLiteral("lavfi=[%1]").arg(chain);
}

} // namespace fc
