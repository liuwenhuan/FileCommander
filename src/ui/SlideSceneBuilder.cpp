#include "SlideSceneBuilder.h"

#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QImage>
#include <QLineF>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QRegExp>
#include <QStringRef>
#include <QTextDocument>
#include <QVector>
#include <QXmlStreamReader>

namespace SlideScene {
namespace {

constexpr double S = kSceneScale;

// Parse an SVG paint value ("#rgb", "#rrggbb", "none", or a couple of names)
// into a QColor. An invalid/empty/"none" value yields an invalid QColor, which
// callers treat as "no fill" / "no stroke".
QColor parseColor(const QStringRef &raw) {
    const QString v = raw.trimmed().toString();
    if (v.isEmpty() || v.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return QColor(); // invalid == no paint
    if (v.startsWith('#')) {
        QColor c(v); // QColor understands #rgb and #rrggbb
        return c;
    }
    // A few named colours office_oxide might emit; QColor(name) covers the rest.
    QColor named(v);
    return named;
}

// Read a numeric SVG attribute, defaulting when absent or malformed.
double attrNum(const QXmlStreamAttributes &attrs, const char *name, double def = 0.0) {
    if (!attrs.hasAttribute(name))
        return def;
    bool ok = false;
    const double v = attrs.value(name).toDouble(&ok);
    return ok ? v : def;
}

// Apply a stroke (colour + width) to a shape's pen, or give it a cosmetic
// no-op pen when the SVG specifies no stroke.
void applyStroke(QAbstractGraphicsShapeItem *item, const QXmlStreamAttributes &attrs) {
    const QColor stroke = parseColor(attrs.value(QLatin1String("stroke")));
    if (stroke.isValid()) {
        const double w = attrNum(attrs, "stroke-width", 0.0) * S;
        QPen pen(stroke);
        pen.setWidthF(w > 0.0 ? w : 0.0); // width 0 == cosmetic hairline
        pen.setCosmetic(w <= 0.0);
        item->setPen(pen);
    } else {
        item->setPen(Qt::NoPen);
    }
}

// Apply a fill; "none"/absent leaves the shape unfilled.
void applyFill(QAbstractGraphicsShapeItem *item, const QXmlStreamAttributes &attrs) {
    const QColor fill = parseColor(attrs.value(QLatin1String("fill")));
    item->setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
}

// <rect x y width height fill stroke stroke-width [rx]>. Rounded corners (rx) are
// drawn via a path item; sharp rects use a plain rect item. Parented to page.
void addRect(const QXmlStreamAttributes &attrs, QGraphicsItem *page) {
    const double x = attrNum(attrs, "x") * S;
    const double y = attrNum(attrs, "y") * S;
    const double w = attrNum(attrs, "width") * S;
    const double h = attrNum(attrs, "height") * S;
    if (w <= 0.0 || h <= 0.0)
        return;
    const double rx = attrNum(attrs, "rx", 0.0) * S;
    if (rx > 0.0) {
        QPainterPath path;
        path.addRoundedRect(QRectF(x, y, w, h), rx, rx);
        auto *item = new QGraphicsPathItem(path, page);
        applyFill(item, attrs);
        applyStroke(item, attrs);
    } else {
        auto *item = new QGraphicsRectItem(QRectF(x, y, w, h), page);
        applyFill(item, attrs);
        applyStroke(item, attrs);
    }
}

// <ellipse cx cy rx ry> / <circle cx cy r>.
void addEllipse(const QXmlStreamAttributes &attrs, QGraphicsItem *page, bool circle) {
    const double cx = attrNum(attrs, "cx") * S;
    const double cy = attrNum(attrs, "cy") * S;
    const double rx = circle ? attrNum(attrs, "r") * S : attrNum(attrs, "rx") * S;
    const double ry = circle ? rx : attrNum(attrs, "ry") * S;
    if (rx <= 0.0 || ry <= 0.0)
        return;
    auto *item = new QGraphicsEllipseItem(QRectF(cx - rx, cy - ry, 2 * rx, 2 * ry), page);
    applyFill(item, attrs);
    applyStroke(item, attrs);
}

// <line x1 y1 x2 y2 stroke stroke-width>.
void addLine(const QXmlStreamAttributes &attrs, QGraphicsItem *page) {
    const double x1 = attrNum(attrs, "x1") * S;
    const double y1 = attrNum(attrs, "y1") * S;
    const double x2 = attrNum(attrs, "x2") * S;
    const double y2 = attrNum(attrs, "y2") * S;
    auto *item = new QGraphicsLineItem(QLineF(x1, y1, x2, y2), page);
    const QColor stroke = parseColor(attrs.value(QLatin1String("stroke")));
    const double w = attrNum(attrs, "stroke-width", 0.0) * S;
    QPen pen(stroke.isValid() ? stroke : QColor(Qt::black));
    pen.setWidthF(w > 0.0 ? w : 0.0);
    pen.setCosmetic(w <= 0.0);
    item->setPen(pen);
}

// Parse a "x,y x,y ..." (or space-separated) point list.
QPolygonF parsePoints(const QStringRef &raw) {
    QPolygonF poly;
    const QString s = raw.toString();
    const QVector<QStringRef> toks =
        s.splitRef(QRegExp(QStringLiteral("[\\s,]+")), QString::SkipEmptyParts);
    for (int i = 0; i + 1 < toks.size(); i += 2) {
        bool okx = false, oky = false;
        const double x = toks[i].toDouble(&okx) * S;
        const double y = toks[i + 1].toDouble(&oky) * S;
        if (okx && oky)
            poly << QPointF(x, y);
    }
    return poly;
}

// <polyline points>/<polygon points>.
void addPoly(const QXmlStreamAttributes &attrs, QGraphicsItem *page, bool close) {
    const QPolygonF poly = parsePoints(attrs.value(QLatin1String("points")));
    if (poly.size() < 2)
        return;
    QPainterPath path;
    path.moveTo(poly.first());
    for (int i = 1; i < poly.size(); ++i)
        path.lineTo(poly[i]);
    if (close)
        path.closeSubpath();
    auto *item = new QGraphicsPathItem(path, page);
    applyFill(item, attrs);
    applyStroke(item, attrs);
}

// Minimal SVG path-data parser: M/L/H/V/C/Q/Z (absolute + relative). Tokenized by
// hand (QRegExp lacks lookahead) into commands and numbers. Anything it can't
// tokenize cleanly is dropped, leaving the parsed prefix — never fatal.
QVector<QString> tokenizePath(const QString &d) {
    QVector<QString> toks;
    QString num;
    auto flush = [&]() {
        if (!num.isEmpty()) {
            toks.push_back(num);
            num.clear();
        }
    };
    for (int i = 0; i < d.size(); ++i) {
        const QChar c = d.at(i);
        if (c.isLetter()) {
            flush();
            toks.push_back(QString(c)); // command
        } else if (c == '-' || c == '+') {
            // A sign starts a new number unless it's an exponent sign (e-3).
            if (!num.isEmpty() && (num.endsWith('e') || num.endsWith('E')))
                num.append(c);
            else {
                flush();
                num.append(c);
            }
        } else if (c.isDigit() || c == '.' || c == 'e' || c == 'E') {
            num.append(c);
        } else { // whitespace or comma: separator
            flush();
        }
    }
    flush();
    return toks;
}

QPainterPath parsePathData(const QString &d) {
    QPainterPath path;
    const QVector<QString> toks = tokenizePath(d);
    QPointF cur(0, 0);
    QPointF start(0, 0);
    int i = 0;
    auto num = [&](bool &ok) -> double {
        if (i >= toks.size()) {
            ok = false;
            return 0.0;
        }
        return toks[i++].toDouble(&ok) * S;
    };
    while (i < toks.size()) {
        const QString &t = toks[i];
        if (t.isEmpty()) {
            ++i;
            continue;
        }
        const QChar cmd = t.at(0);
        if (!cmd.isLetter()) {
            ++i; // stray number without a command; skip
            continue;
        }
        ++i;
        const bool rel = cmd.isLower();
        bool ok = true;
        switch (cmd.toUpper().toLatin1()) {
        case 'M': {
            const double x = num(ok), y = num(ok);
            if (!ok)
                return path;
            cur = rel ? cur + QPointF(x, y) : QPointF(x, y);
            start = cur;
            path.moveTo(cur);
            break;
        }
        case 'L': {
            const double x = num(ok), y = num(ok);
            if (!ok)
                return path;
            cur = rel ? cur + QPointF(x, y) : QPointF(x, y);
            path.lineTo(cur);
            break;
        }
        case 'H': {
            const double x = num(ok);
            if (!ok)
                return path;
            cur = rel ? QPointF(cur.x() + x, cur.y()) : QPointF(x, cur.y());
            path.lineTo(cur);
            break;
        }
        case 'V': {
            const double y = num(ok);
            if (!ok)
                return path;
            cur = rel ? QPointF(cur.x(), cur.y() + y) : QPointF(cur.x(), y);
            path.lineTo(cur);
            break;
        }
        case 'C': {
            const double x1 = num(ok), y1 = num(ok), x2 = num(ok), y2 = num(ok),
                         x = num(ok), y = num(ok);
            if (!ok)
                return path;
            const QPointF c1 = rel ? cur + QPointF(x1, y1) : QPointF(x1, y1);
            const QPointF c2 = rel ? cur + QPointF(x2, y2) : QPointF(x2, y2);
            cur = rel ? cur + QPointF(x, y) : QPointF(x, y);
            path.cubicTo(c1, c2, cur);
            break;
        }
        case 'Q': {
            const double x1 = num(ok), y1 = num(ok), x = num(ok), y = num(ok);
            if (!ok)
                return path;
            const QPointF c1 = rel ? cur + QPointF(x1, y1) : QPointF(x1, y1);
            cur = rel ? cur + QPointF(x, y) : QPointF(x, y);
            path.quadTo(c1, cur);
            break;
        }
        case 'Z':
            path.closeSubpath();
            cur = start;
            break;
        default:
            return path; // unknown command: stop, keep what parsed
        }
    }
    return path;
}

// <path d fill stroke stroke-width>.
void addPath(const QXmlStreamAttributes &attrs, QGraphicsItem *page) {
    const QPainterPath path = parsePathData(attrs.value(QLatin1String("d")).toString());
    if (path.isEmpty())
        return;
    auto *item = new QGraphicsPathItem(path, page);
    applyFill(item, attrs);
    applyStroke(item, attrs);
}

// <image x y width height (xlink:)href="data:image/<fmt>;base64,...">. A decode
// failure is swallowed (no item added) rather than fatal.
void addImage(const QXmlStreamAttributes &attrs, QGraphicsItem *page) {
    const double x = attrNum(attrs, "x") * S;
    const double y = attrNum(attrs, "y") * S;
    const double w = attrNum(attrs, "width") * S;
    const double h = attrNum(attrs, "height") * S;
    if (w <= 0.0 || h <= 0.0)
        return;
    QStringRef href = attrs.value(QLatin1String("http://www.w3.org/1999/xlink"),
                                  QLatin1String("href"));
    if (href.isEmpty())
        href = attrs.value(QLatin1String("href"));
    if (href.isEmpty())
        return;
    const QString uri = href.toString();
    const int comma = uri.indexOf(QLatin1String("base64,"));
    if (comma < 0)
        return;
    const QByteArray b64 = uri.mid(comma + 7).toLatin1();
    const QByteArray bytes = QByteArray::fromBase64(b64);
    QImage img;
    if (!img.loadFromData(bytes))
        return;
    auto *item = new QGraphicsPixmapItem(QPixmap::fromImage(img), page);
    item->setTransformationMode(Qt::SmoothTransformation);
    // Scale the (native-resolution) pixmap into the SVG's target box, then place
    // it. The pixmap item draws in its own pixels, so scale = box / pixel size.
    const double sx = img.width() > 0 ? w / double(img.width()) : 1.0;
    const double sy = img.height() > 0 ? h / double(img.height()) : 1.0;
    item->setScale(qMin(sx, sy)); // uniform scale (matches xMidYMid meet intent)
    item->setPos(x, y);
}

// <text x y font-size fill font-family font-weight font-style text-anchor>txt.
// SVG y is the baseline and font-size is in EMU; a QGraphicsTextItem is anchored
// by its top-left, so top = baseline - ascent and the anchor shifts x by the
// measured text width. Returns the item's text (appended by the caller to the
// slide's plain-text accumulation).
void addText(const QXmlStreamAttributes &attrs, const QString &text, QGraphicsItem *page,
             QString *outText) {
    if (text.trimmed().isEmpty())
        return;
    const double x = attrNum(attrs, "x") * S;
    const double baseline = attrNum(attrs, "y") * S;
    const double sizeEmu = attrNum(attrs, "font-size", 18.0 * 12700.0); // 18pt fallback
    int px = qRound(sizeEmu * S);
    if (px < 1)
        px = 1;

    QString family = attrs.value(QLatin1String("font-family")).toString();
    if (family.isEmpty())
        family = QStringLiteral("sans-serif");
    QFont font(family);
    font.setPixelSize(px);
    if (attrs.value(QLatin1String("font-weight")) == QLatin1String("bold"))
        font.setBold(true);
    if (attrs.value(QLatin1String("font-style")) == QLatin1String("italic"))
        font.setItalic(true);

    const QFontMetricsF fm(font);
    const double advance = fm.horizontalAdvance(text);
    const QStringRef anchor = attrs.value(QLatin1String("text-anchor"));
    double left = x;
    if (anchor == QLatin1String("middle"))
        left = x - advance / 2.0;
    else if (anchor == QLatin1String("end"))
        left = x - advance;
    const double top = baseline - fm.ascent();

    auto *item = new QGraphicsTextItem(page);
    item->document()->setDocumentMargin(0); // no stray inset before the glyphs
    item->setFont(font);
    const QColor fill = parseColor(attrs.value(QLatin1String("fill")));
    item->setDefaultTextColor(fill.isValid() ? fill : QColor(Qt::black));
    item->setPlainText(text);
    item->setPos(left, top);
    item->setTextInteractionFlags(Qt::TextSelectableByMouse);

    if (outText) {
        if (!outText->isEmpty())
            outText->append('\n');
        outText->append(text);
    }
}

} // namespace

QGraphicsItem *buildSlidePage(const QByteArray &svg, QSizeF *outSizeScene, QString *outText) {
    QXmlStreamReader xml(svg);

    // The page background rect is created once the <svg> viewBox is known; every
    // other element is parented to it (its local space is the slide's EMU/100).
    QGraphicsRectItem *page = nullptr;
    QSizeF sizeScene(960 * S, 540 * S);

    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok != QXmlStreamReader::StartElement)
            continue;
        const QStringRef name = xml.name();
        const QXmlStreamAttributes attrs = xml.attributes();

        if (name == QLatin1String("svg")) {
            // viewBox="minX minY W H" gives the slide size; fall back to defaultSize
            // via width/height if absent.
            double w = 960, h = 540;
            const QString vb = attrs.value(QLatin1String("viewBox")).trimmed().toString();
            const QVector<QStringRef> parts =
                vb.splitRef(QRegExp(QStringLiteral("[\\s,]+")), QString::SkipEmptyParts);
            if (parts.size() == 4) {
                w = parts[2].toDouble();
                h = parts[3].toDouble();
            } else {
                w = attrNum(attrs, "width", 960);
                h = attrNum(attrs, "height", 540);
            }
            if (w <= 0)
                w = 960;
            if (h <= 0)
                h = 540;
            sizeScene = QSizeF(w * S, h * S);
            page = new QGraphicsRectItem(QRectF(0, 0, w * S, h * S));
            page->setBrush(QBrush(Qt::white));
            page->setPen(QPen(QColor(0xcc, 0xcc, 0xcc))); // faint page border
            continue;
        }
        if (!page)
            continue; // elements before <svg> (shouldn't happen) have no parent

        if (name == QLatin1String("rect"))
            addRect(attrs, page);
        else if (name == QLatin1String("ellipse"))
            addEllipse(attrs, page, false);
        else if (name == QLatin1String("circle"))
            addEllipse(attrs, page, true);
        else if (name == QLatin1String("line"))
            addLine(attrs, page);
        else if (name == QLatin1String("polyline"))
            addPoly(attrs, page, false);
        else if (name == QLatin1String("polygon"))
            addPoly(attrs, page, true);
        else if (name == QLatin1String("path"))
            addPath(attrs, page);
        else if (name == QLatin1String("image"))
            addImage(attrs, page);
        else if (name == QLatin1String("text"))
            addText(attrs, xml.readElementText(), page, outText);
        // Unknown elements: ignored (graceful degradation).
    }

    if (xml.hasError() && !page) {
        // Structurally broken SVG with no usable page: let the caller degrade.
        return nullptr;
    }
    if (outSizeScene)
        *outSizeScene = sizeScene;
    return page;
}

// Unescape the five XML predefined entities in a small text fragment.
QString xmlUnescape(const QString &s) {
    QString out = s;
    out.replace(QLatin1String("&lt;"), QLatin1String("<"));
    out.replace(QLatin1String("&gt;"), QLatin1String(">"));
    out.replace(QLatin1String("&quot;"), QLatin1String("\""));
    out.replace(QLatin1String("&apos;"), QLatin1String("'"));
    out.replace(QLatin1String("&amp;"), QLatin1String("&")); // last, so &amp;lt; survives
    return out;
}

void parseSlideMeta(const QByteArray &svg, QSizeF *outSizeScene, QString *outText) {
    // A byte-level scan rather than a full XML parse: an image-heavy deck embeds
    // megabytes of base64 in <image href=...>, and QXmlStreamReader tokenizes all
    // of it. We only need the viewBox (near the head) and the <text> contents, so
    // indexOf jumps straight over the base64 blobs -- orders of magnitude cheaper.
    QSizeF sizeScene(960 * S, 540 * S);

    // viewBox="minX minY W H" — parse width/height from the first occurrence.
    const int vb = svg.indexOf("viewBox=\"");
    if (vb >= 0) {
        const int start = vb + 9;
        const int end = svg.indexOf('"', start);
        if (end > start) {
            const QString vals = QString::fromLatin1(svg.constData() + start, end - start);
            const QVector<QStringRef> parts =
                vals.splitRef(QRegExp(QStringLiteral("[\\s,]+")), QString::SkipEmptyParts);
            if (parts.size() == 4) {
                const double w = parts[2].toDouble();
                const double h = parts[3].toDouble();
                if (w > 0 && h > 0)
                    sizeScene = QSizeF(w * S, h * S);
            }
        }
    }
    if (outSizeScene)
        *outSizeScene = sizeScene;
    if (!outText)
        return;

    // Collect every <text ...>inner</text>. Our SVG never nests elements inside
    // <text>, so a flat scan for the closing tag is sufficient.
    int pos = 0;
    while (true) {
        const int lt = svg.indexOf("<text", pos);
        if (lt < 0 || lt + 5 >= svg.size())
            break;
        const char after = svg.at(lt + 5); // guard against a hypothetical <textPath>
        if (after != ' ' && after != '>' && after != '\t' && after != '\n') {
            pos = lt + 5;
            continue;
        }
        const int gt = svg.indexOf('>', lt);
        if (gt < 0)
            break;
        const int close = svg.indexOf("</text>", gt);
        if (close < 0)
            break;
        const QByteArray inner = svg.mid(gt + 1, close - gt - 1);
        const QString t = xmlUnescape(QString::fromUtf8(inner));
        if (!t.trimmed().isEmpty()) {
            if (!outText->isEmpty())
                outText->append('\n');
            outText->append(t);
        }
        pos = close + 7;
    }
}

} // namespace SlideScene
