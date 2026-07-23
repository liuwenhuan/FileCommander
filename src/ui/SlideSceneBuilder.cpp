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
#include <QHash>
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
#include <QTextOption>
#include <QTransform>
#include <QVector>
#include <QXmlStreamReader>

#include <algorithm>

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

// Apply an SVG stroke-dasharray to a pen. Qt expresses dash lengths as multiples
// of the pen width, and office_oxide emits dasharray in the same user units as
// stroke-width, so dividing by stroke-width gives the scale-independent pattern
// (e.g. dasharray "101600 76200" at stroke-width 25400 -> [4, 3]).
void applyDash(QPen &pen, const QXmlStreamAttributes &attrs) {
    const QString da = attrs.value(QLatin1String("stroke-dasharray")).toString();
    if (da.isEmpty() || da == QLatin1String("none"))
        return;
    const double sw = attrNum(attrs, "stroke-width", 0.0);
    const double unit = sw > 0.0 ? sw : 1.0;
    QVector<qreal> pat;
    const QStringList toks =
        da.split(QRegExp(QStringLiteral("[\\s,]+")), QString::SkipEmptyParts);
    for (const QString &t : toks) {
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (ok)
            pat.append(qMax(0.01, v / unit));
    }
    if (pat.size() % 2 == 1)
        pat += pat; // SVG repeats an odd-length list to make it even
    if (!pat.isEmpty())
        pen.setDashPattern(pat);
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
        applyDash(pen, attrs);
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

// Parse an SVG transform list. oxide only emits rotate(deg cx cy) for shape/group
// rotation; anything else yields identity. The center (cx,cy) is in EMU, so it is
// scaled by S to match the scene units every element is drawn in.
QTransform parseSvgTransform(const QString &spec) {
    QTransform t;
    QRegExp re(QStringLiteral("rotate\\(([^)]*)\\)"));
    if (re.indexIn(spec) < 0)
        return t;
    const QVector<QStringRef> p = re.cap(1).splitRef(
        QRegExp(QStringLiteral("[\\s,]+")), QString::SkipEmptyParts);
    if (p.isEmpty())
        return t;
    const double deg = p[0].toDouble();
    double cx = 0.0, cy = 0.0;
    if (p.size() >= 3) {
        cx = p[1].toDouble() * S;
        cy = p[2].toDouble() * S;
    }
    t.translate(cx, cy);
    t.rotate(deg);
    t.translate(-cx, -cy);
    return t;
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
    applyDash(pen, attrs);
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
//
// PowerPoint picture crops (a:srcRect) arrive as data-crop-l/t/r/b: the fraction
// of the source image trimmed off each edge, expressed as an integer per-mille of
// 100000 (so 12500 == 12.5% trimmed off that edge; absent == 0). We copy() the
// cropped sub-rect out of the decoded pixmap, then scale THAT sub-image to fill the
// SVG's target box exactly (non-uniform) -- the whole point of a crop is that the
// retained region maps onto the shape box, so letterboxing it would be wrong.
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
    QPixmap pm = QPixmap::fromImage(img);

    // Crop fractions (per-mille of 100000) trimmed off each edge; default 0.
    const double lf = attrNum(attrs, "data-crop-l", 0.0) / 100000.0;
    const double tf = attrNum(attrs, "data-crop-t", 0.0) / 100000.0;
    const double rf = attrNum(attrs, "data-crop-r", 0.0) / 100000.0;
    const double bf = attrNum(attrs, "data-crop-b", 0.0) / 100000.0;
    const bool cropped = (lf > 0.0 || tf > 0.0 || rf > 0.0 || bf > 0.0) &&
                         (lf + rf < 1.0) && (tf + bf < 1.0);
    if (cropped) {
        const int iw = pm.width(), ih = pm.height();
        const int cx = qRound(iw * lf);
        const int cy = qRound(ih * tf);
        const int cw = qRound(iw * (1.0 - lf - rf));
        const int ch = qRound(ih * (1.0 - tf - bf));
        if (cw > 0 && ch > 0)
            pm = pm.copy(cx, cy, cw, ch);
    }

    auto *item = new QGraphicsPixmapItem(pm, page);
    item->setTransformationMode(Qt::SmoothTransformation);
    // Scale the (possibly cropped) pixmap into the SVG's target box. The pixmap item
    // draws in its own pixels, so scale = box / pixel size. A crop fills the box
    // exactly (non-uniform); an uncropped image keeps the xMidYMid-meet uniform scale.
    const double sx = pm.width() > 0 ? w / double(pm.width()) : 1.0;
    const double sy = pm.height() > 0 ? h / double(pm.height()) : 1.0;
    if (cropped) {
        item->setTransform(QTransform::fromScale(sx, sy));
    } else {
        item->setScale(qMin(sx, sy)); // uniform scale (matches xMidYMid meet intent)
    }
    item->setPos(x, y);
}

// <text x y font-size fill font-family font-weight font-style text-anchor>txt.
// SVG y is the baseline and font-size is in EMU; a QGraphicsTextItem is anchored
// by its top-left, so top = baseline - ascent and the anchor shifts x by the
// measured text width. Returns the item's text (appended by the caller to the
// slide's plain-text accumulation).
//
// One text item created per <text> (a source paragraph). Its data-box id (shared
// by paragraphs in the same PowerPoint text box) and the item are recorded in
// `entries` so buildSlidePage can reflow same-box paragraphs afterwards -- a
// paragraph that word-wraps taller must push the following same-box paragraphs
// down instead of overlapping them.
struct TextEntry {
    int box;                 // data-box id (-1 == none; excluded from reflow)
    QGraphicsTextItem *item;
    double vcenter;          // data-vc (scene units); <0 == not vertically centred
};

void addText(const QXmlStreamAttributes &attrs, const QString &text, QGraphicsItem *page,
             QString *outText, QVector<TextEntry> *entries) {
    if (text.trimmed().isEmpty())
        return;
    const double x = attrNum(attrs, "x") * S;
    const double baseline = attrNum(attrs, "y") * S;
    const double sizeEmu = attrNum(attrs, "font-size", 18.0 * 12700.0); // 18pt fallback
    int px = qRound(sizeEmu * S);
    if (px < 1)
        px = 1;

    // font-family may be a CJK-first fallback list ("EA字体, latin字体") from oxide;
    // split on commas and hand the whole list to QFont so it falls back family-by-
    // family (a bare QFont(str) would treat the entire string as one missing name).
    QStringList families;
    const QStringList rawFamilies =
        attrs.value(QLatin1String("font-family")).toString().split(QLatin1Char(','));
    for (const QString &f : rawFamilies) {
        const QString t = f.trimmed();
        if (!t.isEmpty())
            families << t;
    }
    if (families.isEmpty())
        families << QStringLiteral("sans-serif");
    QFont font;
    font.setFamilies(families);
    font.setPixelSize(px);
    if (attrs.value(QLatin1String("font-weight")) == QLatin1String("bold"))
        font.setBold(true);
    if (attrs.value(QLatin1String("font-style")) == QLatin1String("italic"))
        font.setItalic(true);
    // oxide flags run-level underline as text-decoration="underline" (subscript /
    // superscript it bakes into y + font-size, so nothing to do here).
    if (attrs.value(QLatin1String("text-decoration")) == QLatin1String("underline"))
        font.setUnderline(true);

    const QFontMetricsF fm(font);
    const QStringRef anchor = attrs.value(QLatin1String("text-anchor"));
    // SVG y is the baseline; a QGraphicsTextItem is anchored by its top-left, so the
    // item top puts the first line's baseline back on y (document margin is 0).
    const double top = baseline - fm.ascent();

    auto *item = new QGraphicsTextItem(page);
    item->document()->setDocumentMargin(0); // no stray inset before the glyphs
    item->setFont(font);
    const QColor fill = parseColor(attrs.value(QLatin1String("fill")));
    item->setDefaultTextColor(fill.isValid() ? fill : QColor(Qt::black));
    item->setPlainText(text);
    item->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // A whole source paragraph arrives as one <text>. When oxide knows the text
    // box's usable width it emits data-w (EMU, already group-transformed); we then
    // let QGraphicsTextItem word-wrap at that width using real font metrics, with
    // text-anchor driving both the box's horizontal placement and the in-box
    // alignment. x stays the SVG anchor point (start=left, middle=centre, end=right
    // of the box), same as the non-wrapping case -- so the box's left edge is
    // derived from x, the anchor, and the width. Without data-w the text stays
    // single-line (short labels), using the measured-advance left-shift as before.
    const double dataW = attrNum(attrs, "data-w", -1.0) * S;
    if (dataW > 0.0) {
        item->setTextWidth(dataW);
        QTextOption opt = item->document()->defaultTextOption();
        // Break within the box even for a long unbroken run (URLs, CJK) so text
        // never spills past the box edge.
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        // If even a single glyph is wider than the wrap box, Qt lays that
        // overflowing line out from the box's left edge -- it cannot centre a
        // line wider than the box -- which shoves centred/right text off the
        // box centre. This happens for an oversized glyph in a tiny box (a
        // numeral centred in a small circle) and for per-character CJK wrapping.
        // Centre/right on the widest glyph's own width in that case so the ink
        // stays on the anchor point instead of drifting right.
        double widestChar = 0.0;
        for (const QChar &c : text)
            widestChar = qMax(widestChar, fm.horizontalAdvance(c));
        const double refW = qMax(dataW, widestChar);
        Qt::Alignment align = Qt::AlignLeft;
        double left = x; // anchor=start: box left edge is the anchor point
        if (anchor == QLatin1String("middle")) {
            align = Qt::AlignHCenter;
            left = x - refW / 2.0;
        } else if (anchor == QLatin1String("end")) {
            align = Qt::AlignRight;
            left = x - refW;
        }
        opt.setAlignment(align);
        item->document()->setDefaultTextOption(opt);
        item->setPos(left, top);
    } else {
        const double advance = fm.horizontalAdvance(text);
        double left = x;
        if (anchor == QLatin1String("middle"))
            left = x - advance / 2.0;
        else if (anchor == QLatin1String("end"))
            left = x - advance;
        item->setPos(left, top);
    }

    if (outText) {
        if (!outText->isEmpty())
            outText->append('\n');
        outText->append(text);
    }

    if (entries) {
        bool hasBox = false;
        const int box = attrs.value(QLatin1String("data-box")).toInt(&hasBox);
        // data-vc (EMU box centre Y) marks a vertically-centred box: oxide bakes
        // the baseline assuming no wrap, so reflowTextBoxes re-centres the block
        // after ttc word-wraps it. Absent (< 0) for top/bottom-anchored boxes.
        const double vc = attrNum(attrs, "data-vc", -1.0);
        entries->push_back({hasBox ? box : -1, item, vc >= 0.0 ? vc * S : -1.0});
    }
}

// Push same-box paragraphs down so a word-wrapped (taller) paragraph never
// overlaps the next paragraph in the same PowerPoint text box. oxide positions
// each paragraph's baseline assuming it does not wrap, so wrapping steals vertical
// space from whatever follows in the box. For each data-box group (in original
// top order) we track how much each paragraph overran the gap oxide reserved for
// it and shift every later paragraph down by that cumulative overflow -- so a box
// with no wrapping is left exactly as authored, and one with wrapping expands only
// as much as the wrap required. Different boxes are independent.
void reflowTextBoxes(const QVector<TextEntry> &entries) {
    QHash<int, QVector<QGraphicsTextItem *>> groups;
    QHash<int, double> boxVCenter; // box id -> data-vc (scene units), <0 if none
    for (const TextEntry &e : entries) {
        if (e.box >= 0) {
            groups[e.box].push_back(e.item);
            if (e.vcenter >= 0.0)
                boxVCenter[e.box] = e.vcenter;
        }
    }
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QVector<QGraphicsTextItem *> &items = it.value();
        std::sort(items.begin(), items.end(),
                  [](QGraphicsTextItem *a, QGraphicsTextItem *b) { return a->y() < b->y(); });

        // Push same-box paragraphs down so a word-wrapped (taller) paragraph
        // never overlaps the next paragraph in the same box.
        if (items.size() >= 2) {
            QVector<double> origTop;
            origTop.reserve(items.size());
            for (QGraphicsTextItem *it2 : items)
                origTop.push_back(it2->y());

            double shift = 0.0;
            for (int i = 1; i < items.size(); ++i) {
                const double reserved = origTop[i] - origTop[i - 1]; // space oxide gave prev
                const double actual = items[i - 1]->boundingRect().height();
                shift += qMax(0.0, actual - reserved); // only when prev overran its slot
                items[i]->setY(origTop[i] + shift);
            }
        }

        // Re-centre a vertically-centred box after wrapping. oxide bakes the
        // baseline assuming each <text> is one line and centres that assumed
        // block on the box centre. When ttc word-wraps a line into N lines the
        // block grows downward (push-down above expands the bottom), so its
        // centre drifts below the box centre by half the total wrap overflow.
        // Shift the whole group up by that half so the real block re-centres --
        // a no-wrap box has zero overflow and stays exactly as oxide authored,
        // preserving oxide's single-line ink correction.
        if (boxVCenter.value(it.key(), -1.0) >= 0.0) {
            double extra = 0.0;
            for (QGraphicsTextItem *g : items) {
                const QFontMetricsF gfm(g->font());
                extra += qMax(0.0, g->boundingRect().height() - gfm.lineSpacing());
            }
            const double shiftUp = extra / 2.0;
            if (shiftUp > 0.0) {
                for (QGraphicsTextItem *g : items)
                    g->setY(g->y() - shiftUp);
            }
        }
    }
}

} // namespace

QGraphicsItem *buildSlidePage(const QByteArray &svg, QSizeF *outSizeScene, QString *outText) {
    QXmlStreamReader xml(svg);

    // The page background rect is created once the <svg> viewBox is known; every
    // other element is parented to it (its local space is the slide's EMU/100).
    QGraphicsRectItem *page = nullptr;
    QSizeF sizeScene(960 * S, 540 * S);
    QVector<TextEntry> textEntries; // for post-parse same-box paragraph reflow
    // Parent stack: each element is parented to the current top. A <g transform>
    // pushes a rotation container so its children inherit the rotate(); a plain <g>
    // re-pushes the same top so the EndElement pop stays balanced.
    QVector<QGraphicsItem *> parents;

    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::EndElement) {
            if (xml.name() == QLatin1String("g") && parents.size() > 1)
                parents.pop_back();
            continue;
        }
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
            parents.append(page);
            continue;
        }
        if (!page)
            continue; // elements before <svg> (shouldn't happen) have no parent

        if (name == QLatin1String("g")) {
            QGraphicsItem *top = parents.last();
            const QString tf = attrs.value(QLatin1String("transform")).toString();
            if (!tf.isEmpty()) {
                const QTransform mat = parseSvgTransform(tf);
                if (!mat.isIdentity()) {
                    auto *cont = new QGraphicsRectItem(top);
                    cont->setPen(Qt::NoPen);
                    cont->setBrush(Qt::NoBrush);
                    cont->setTransform(mat);
                    parents.append(cont);
                    continue;
                }
            }
            parents.append(top); // no-op group: keep pop pairing on EndElement
            continue;
        }

        QGraphicsItem *parent = parents.last();
        if (name == QLatin1String("rect"))
            addRect(attrs, parent);
        else if (name == QLatin1String("ellipse"))
            addEllipse(attrs, parent, false);
        else if (name == QLatin1String("circle"))
            addEllipse(attrs, parent, true);
        else if (name == QLatin1String("line"))
            addLine(attrs, parent);
        else if (name == QLatin1String("polyline"))
            addPoly(attrs, parent, false);
        else if (name == QLatin1String("polygon"))
            addPoly(attrs, parent, true);
        else if (name == QLatin1String("path"))
            addPath(attrs, parent);
        else if (name == QLatin1String("image"))
            addImage(attrs, parent);
        else if (name == QLatin1String("text"))
            addText(attrs, xml.readElementText(), parent, outText, &textEntries);
        // Unknown elements: ignored (graceful degradation).
    }

    if (xml.hasError() && !page) {
        // Structurally broken SVG with no usable page: let the caller degrade.
        return nullptr;
    }
    // Now that every paragraph's wrapped height is known, cascade same-box
    // paragraphs downward so wrapping never overlaps the next paragraph.
    reflowTextBoxes(textEntries);
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

QStringList parseSlideTexts(const QByteArray &svg) {
    // Same flat <text> scan as parseSlideMeta, but each non-empty paragraph is
    // returned separately (and unclipped) rather than concatenated, so its index
    // lines up with the slide's built text items.
    QStringList out;
    int pos = 0;
    while (true) {
        const int lt = svg.indexOf("<text", pos);
        if (lt < 0 || lt + 5 >= svg.size())
            break;
        const char after = svg.at(lt + 5); // guard against <textPath>
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
        if (!t.trimmed().isEmpty())
            out << t;
        pos = close + 7;
    }
    return out;
}

} // namespace SlideScene
