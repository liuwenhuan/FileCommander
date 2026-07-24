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
#include <QGraphicsSimpleTextItem>
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
#include <QPair>
#include <QStringRef>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
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
    QColor stroke = parseColor(attrs.value(QLatin1String("stroke")));
    if (stroke.isValid()) {
        // B1: stroke-opacity (0-1) modulates the pen colour's alpha. Absent leaves
        // the colour fully opaque, so default behaviour is unchanged.
        bool opOk = false;
        const double op = attrs.value(QLatin1String("stroke-opacity")).toDouble(&opOk);
        if (opOk)
            stroke.setAlphaF(qBound(0.0, op, 1.0));
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
    QColor fill = parseColor(attrs.value(QLatin1String("fill")));
    if (fill.isValid()) {
        // B1: fill-opacity (0-1) modulates the fill colour's alpha so a semi-
        // transparent overlay (e.g. a 14% srgbClr wash) no longer paints as a
        // solid block that hides content beneath it. Absent == fully opaque, so
        // solid fills are untouched (zero regression).
        bool opOk = false;
        const double op = attrs.value(QLatin1String("fill-opacity")).toDouble(&opOk);
        if (opOk)
            fill.setAlphaF(qBound(0.0, op, 1.0));
    }
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

// One text run: its characters, the fill colour to draw them in (an invalid QColor
// means "use the <text>'s default fill"), and an optional font size in EMU (<=0 ==
// "use the <text>'s default font size"). A <text> with no <tspan> yields a single
// default-coloured, default-sized segment; a <tspan> (#19 colour, B2b bullet size)
// splits off a run carrying its own fill and/or font-size.
struct TextSegment {
    QString text;
    QColor color;        // invalid == <text> default fill
    double fontSizeEmu;  // <=0 == <text> default font size
    int baselineShift;   // data-baseline permille (0 == normal; >0 super, <0 sub)
};

// Read a <text> element's contents into runs. The reader is positioned on the
// <text> StartElement; on return it has consumed the matching </text>. Characters
// accumulate into the current run; a <tspan fill=.. font-size=..> opens a new run
// carrying that colour/size, its </tspan> closes it back to the <text> defaults.
// (readElementText() cannot be used here -- it raises an error on the <tspan> child.)
// Only <text>'s direct <tspan> children are honoured (no nesting), per the contract.
QVector<TextSegment> readTextSegments(QXmlStreamReader &xml) {
    QVector<TextSegment> segs;
    QString cur;
    QColor curColor;         // invalid == default fill
    double curSize = 0.0;    // <=0 == default font size
    int curShift = 0;        // data-baseline permille (0 == normal)
    auto flush = [&]() {
        if (!cur.isEmpty()) {
            segs.push_back({cur, curColor, curSize, curShift});
            cur.clear();
        }
    };
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::Characters) {
            cur.append(xml.text());
        } else if (tok == QXmlStreamReader::StartElement &&
                   xml.name() == QLatin1String("tspan")) {
            flush(); // close the preceding default run
            const QXmlStreamAttributes ta = xml.attributes();
            curColor = parseColor(ta.value(QLatin1String("fill")));
            // B2b: a bullet marker (or any tspan) may carry its own font-size in EMU
            // so its glyph is sized independently of the surrounding paragraph.
            bool okSize = false;
            const double sz = ta.value(QLatin1String("font-size")).toDouble(&okSize);
            curSize = okSize ? sz : 0.0;
            // Contract v2 super/subscript: data-baseline is a permille offset
            // (30000 == +30% of ascent up; -25000 == -25% down). oxide keeps the
            // run's parent font-size; ttc shrinks + shifts it (see layoutTextBox).
            bool okBl = false;
            const int bl = ta.value(QLatin1String("data-baseline")).toInt(&okBl);
            curShift = okBl ? bl : 0;
        } else if (tok == QXmlStreamReader::EndElement &&
                   xml.name() == QLatin1String("tspan")) {
            flush();             // close this run
            curColor = QColor();  // back to the <text> default colour
            curSize = 0.0;        // back to the <text> default font size
            curShift = 0;         // back to the baseline
        } else if (tok == QXmlStreamReader::EndElement &&
                   xml.name() == QLatin1String("text")) {
            flush();
            break;
        }
    }
    return segs;
}

// Build the QFont for a <text> from its SVG attributes: the CJK-first family
// fallback list, pixel size (font-size EMU * S), and bold/italic/underline flags.
// Shared by the horizontal and vertical text paths. font-size is in EMU; the
// scene works in EMU/100, so px = EMU * S.
QFont buildTextFont(const QXmlStreamAttributes &attrs) {
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
    return font;
}

// ---------------------------------------------------------------------------
// Contract-v2 layout layer (ttc is the sole typesetter).
//
// When oxide emits the v2 text contract (data-anchor / data-para present) it no
// longer bakes a baseline y: it hands each paragraph's text box geometry and
// leaves ALL layout to ttc -- line wrapping (with CJK kinsoku), line pitch,
// vertical box anchoring, horizontal alignment, and super/subscript -- every one
// computed here from real QFontMetricsF. The pre-v2 path (baked baseline y +
// reflowTextBoxes) is untouched, so a deck from old oxide renders exactly as
// before (zero regression); only v2 text takes the box engine below.
// ---------------------------------------------------------------------------

// Global vertical metric compensation. A Windows/Office font (e.g. 微软雅黑) is
// substituted on Linux by fontconfig (-> 思源黑体/Source Han Sans), whose ascent
// and line height differ slightly, so a whole paragraph can sit a touch high or
// its lines too loose/tight versus PowerPoint. This table nudges the SUBSTITUTE
// font's ascent/line-height back toward the authored font's proportions.
//
//   ascent : multiplies the measured ascent (shifts the first baseline down when
//            >1) so the block's optical top matches Office.
//   line   : multiplies the measured line height (the inter-line pitch).
//
// Keyed by the *authored* family (lower-cased, as it appears in font-family).
// No entry  ->  {1,1} == use the substitute font's own metrics untouched (the
// safe default the task calls for). ADD ENTRIES HERE as Office comparisons show
// a consistent offset for a given source font -- values are empirical and belong
// in this one obvious place.
struct FontComp { double ascent; double line; };
FontComp fontCompFor(const QString &familyRaw) {
    static const QHash<QString, FontComp> table = {
        // family (lower-case)          ascent   line     (1,1 == neutral)
        {QStringLiteral("微软雅黑"),        {1.00, 1.00}},
        {QStringLiteral("microsoft yahei"), {1.00, 1.00}},
        {QStringLiteral("等线"),            {1.00, 1.00}},
        {QStringLiteral("dengxian"),        {1.00, 1.00}},
        {QStringLiteral("宋体"),            {1.00, 1.00}},
        {QStringLiteral("simsun"),          {1.00, 1.00}},
        {QStringLiteral("黑体"),            {1.00, 1.00}},
        {QStringLiteral("simhei"),          {1.00, 1.00}},
        {QStringLiteral("calibri"),         {1.00, 1.00}},
    };
    const auto it = table.constFind(familyRaw.trimmed().toLower());
    return it != table.constEnd() ? it.value() : FontComp{1.0, 1.0};
}

// The first (primary) family named in a font-family list -- the one used to look
// up the compensation table (oxide lists the EA/CJK family first).
QString primaryFamily(const QXmlStreamAttributes &attrs) {
    const QString raw = attrs.value(QLatin1String("font-family")).toString();
    const int comma = raw.indexOf(QLatin1Char(','));
    return (comma >= 0 ? raw.left(comma) : raw).trimmed();
}

// --- CJK line-break rules (kinsoku shori / 禁则处理) -----------------------
// Characters that may not START a line (closing brackets, trailing punctuation):
// when a break would put one at a line head, it "hangs" onto the previous line.
bool isHeadForbidden(QChar c) {
    static const QString set = QStringLiteral(
        "，。、．：；！？）」』】》〉〕｝］’”､…‥・ー々ぁぃぅぇぉっゃゅょゎ"
        "ァィゥェォッャュョヮ)]},.;:!?");
    return set.contains(c);
}
// Characters that may not END a line (opening brackets): a break landing right
// after one pushes it down to the next line instead.
bool isTailForbidden(QChar c) {
    static const QString set = QStringLiteral("（「『【《〈〔｛［‘“([{");
    return set.contains(c);
}

// Character class for break-opportunity decisions.
enum class CClass { Space, Alnum, Cjk, Other };
CClass classOf(QChar c) {
    if (c.isSpace())
        return CClass::Space;
    const ushort u = c.unicode();
    // Latin letters/digits and the punctuation that binds a "word" together
    // (so 12,345.6 / http://a.b / e-mail are not split mid-token).
    if ((u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
        c == QLatin1Char('.') || c == QLatin1Char(',') || c == QLatin1Char('/') ||
        c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('@') ||
        c == QLatin1Char(':') || c == QLatin1Char('%') || c == QLatin1Char('+'))
        return CClass::Alnum;
    // CJK unified ideographs + common kana + fullwidth ranges wrap per glyph.
    if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF) ||
        (u >= 0x3040 && u <= 0x30FF) || (u >= 0xFF00 && u <= 0xFFEF) ||
        (u >= 0x3000 && u <= 0x303F))
        return CClass::Cjk;
    return CClass::Other;
}

// One flattened glyph carrying the run it came from (for per-run font metrics).
struct Glyph { QChar ch; int run; };

// A run within a paragraph: its resolved font, colour and super/subscript.
struct LineRun { QString text; QColor color; QFont font; int shift; };

// A wrapped line: the runs that compose it, its total advance and the ascent to
// place it by (max run ascent so a bigger glyph on the line still sits right).
struct LaidLine { QVector<LineRun> runs; double width; double ascent; };

// Whether a break is allowed *before* glyph i (i>0) given its neighbours.
bool breakBefore(const QVector<Glyph> &g, int i) {
    const CClass a = classOf(g[i - 1].ch);
    const CClass b = classOf(g[i].ch);
    if (a == CClass::Space || b == CClass::Space)
        return true;                       // always breakable at whitespace
    if (a == CClass::Alnum && b == CClass::Alnum)
        return false;                      // keep an alnum word intact
    return true;                           // CJK anywhere / across script boundary
}

// Nudge a proposed break index (first glyph of the next line) to satisfy kinsoku:
// pull a head-forbidden char back onto this line, or push a tail-forbidden char
// down to the next. Bounded so it can never collapse a line to nothing.
int kinsoku(const QVector<Glyph> &g, int lineStart, int brk) {
    const int n = g.size();
    for (int guard = 0; guard < 4; ++guard) {
        bool changed = false;
        // Head rule: glyph at brk cannot open a line -> hang it on this line.
        if (brk < n && brk > lineStart && isHeadForbidden(g[brk].ch)) {
            ++brk;
            changed = true;
        }
        // Tail rule: glyph at brk-1 cannot close a line -> drop it to the next.
        if (brk - 1 > lineStart && isTailForbidden(g[brk - 1].ch)) {
            --brk;
            changed = true;
        }
        if (!changed)
            break;
    }
    return qBound(lineStart + 1, brk, n);
}

// A paragraph resolved from one v2 <text>: box geometry + its runs, held until
// buildSlidePage has read the whole box so the box can be laid out as a unit.
struct V2Para {
    QGraphicsItem *parent;
    int box;
    int para;
    double x;                  // anchor x (scene)
    Qt::Alignment halign;      // from text-anchor
    double top;                // data-top (scene) -- box text-area top
    double areaH;              // data-areah (scene) -- box text-area height
    QChar vanchor;             // 't' / 'c' / 'b'
    double dataW;              // wrap width (scene); <=0 == no wrap
    double lineFactor;         // proportional line spacing (1.0 == single)
    double lineFixed;          // fixed line pitch (scene); <=0 == use factor
    double spcBefore;          // space before (scene)
    double spcAfter;           // space after (scene)
    double marL;               // left margin (scene)
    double indent;             // first-line indent delta (scene)
    QFont font;                // paragraph base font
    QColor defColor;
    FontComp comp;             // metric compensation for the base family
    QVector<TextSegment> segs; // runs (colour / size / baseline)
};

// Break a paragraph's runs into wrapped lines using real metrics + kinsoku.
QVector<LaidLine> wrapParagraph(const V2Para &p) {
    // Flatten runs to glyphs and build the resolved per-run font/colour.
    QVector<Glyph> glyphs;
    QVector<QFont> runFont;
    QVector<QColor> runColor;
    QVector<int> runShift;
    for (int r = 0; r < p.segs.size(); ++r) {
        const TextSegment &s = p.segs[r];
        QFont f = p.font;
        if (s.fontSizeEmu > 0.0) {
            int px = qRound(s.fontSizeEmu * S);
            f.setPixelSize(px < 1 ? 1 : px);
        }
        if (s.baselineShift != 0) {
            // Super/subscript run: shrink to ~62% (Office default) around its own
            // size; the shift itself is applied when placing (needs the baseline).
            int px = qMax(1, qRound(f.pixelSize() * 0.62));
            f.setPixelSize(px);
        }
        runFont.push_back(f);
        runColor.push_back(s.color.isValid() ? s.color : p.defColor);
        runShift.push_back(s.baselineShift);
        for (const QChar &ch : s.text)
            glyphs.push_back({ch, r});
    }
    QVector<QFontMetricsF> fm;
    fm.reserve(runFont.size());
    for (const QFont &f : runFont)
        fm.push_back(QFontMetricsF(f));

    auto adv = [&](int i) { return fm[glyphs[i].run].horizontalAdvance(glyphs[i].ch); };

    // Greedy wrap into [start,end) glyph ranges.
    QVector<QPair<int, int>> ranges;
    const int n = glyphs.size();
    const double maxW = p.dataW;
    int lineStart = 0;
    double w = 0.0;
    int lastBreak = -1; // last index a break is allowed before, within this line
    for (int i = 0; i < n; ++i) {
        if (glyphs[i].ch == QLatin1Char('\n')) { // forced break
            ranges.push_back({lineStart, i});
            lineStart = i + 1;
            w = 0.0;
            lastBreak = -1;
            continue;
        }
        const double a = adv(i);
        // Wrap when the box width is exceeded -- but only if this line actually has
        // a legal break point. An unbreakable token that starts the line (e.g. the
        // digits "2006" in a narrow timeline label) is allowed to OVERFLOW rather
        // than be sliced mid-word: Word/Office never break inside a number/word, and
        // slicing "2006" into "200"+"6" is a fidelity bug. Once a later break point
        // appears (script boundary, space, CJK) the line wraps there instead.
        if (maxW > 0.0 && i > lineStart && w + a > maxW && lastBreak > lineStart) {
            int brk = kinsoku(glyphs, lineStart, lastBreak);
            ranges.push_back({lineStart, brk});
            lineStart = brk;
            // Re-measure the glyphs carried onto the new line up to (not incl.) i.
            w = 0.0;
            for (int k = lineStart; k < i; ++k)
                w += adv(k);
            lastBreak = -1;
        }
        if (i > lineStart && breakBefore(glyphs, i))
            lastBreak = i;
        w += a;
    }
    if (lineStart < n || ranges.isEmpty())
        ranges.push_back({lineStart, n});

    // Materialise each range into runs (grouping consecutive same-run glyphs) and
    // measure the line's advance + ascent.
    QVector<LaidLine> lines;
    for (const auto &rg : ranges) {
        LaidLine line;
        line.width = 0.0;
        line.ascent = 0.0;
        int i = rg.first;
        // Skip a single leading space left by a wrap (keeps wrapped lines flush).
        if (i < rg.second && i > 0 && glyphs[i].ch == QLatin1Char(' '))
            ++i;
        while (i < rg.second) {
            const int r = glyphs[i].run;
            QString txt;
            while (i < rg.second && glyphs[i].run == r) {
                txt.append(glyphs[i].ch);
                ++i;
            }
            LineRun lr{txt, runColor[r], runFont[r], runShift[r]};
            line.runs.push_back(lr);
            line.width += fm[r].horizontalAdvance(txt);
            line.ascent = qMax(line.ascent, fm[r].ascent());
        }
        if (line.ascent <= 0.0)
            line.ascent = QFontMetricsF(p.font).ascent();
        lines.push_back(line);
    }
    if (lines.isEmpty())
        lines.push_back(LaidLine{{}, 0.0, QFontMetricsF(p.font).ascent()});
    return lines;
}

// Render one wrapped line at a known baseline. A line free of super/subscript is
// a single selectable QGraphicsTextItem (runs inserted for colour/size). A line
// carrying a shifted run is drawn as per-run QGraphicsSimpleTextItem so each run
// sits at its exact (possibly raised/lowered) baseline -- selection is traded for
// precise placement on those rare lines.
void emitLine(const LaidLine &line, double left, double baselineY, double baseAscent,
              QGraphicsItem *parent) {
    bool anyShift = false;
    for (const LineRun &r : line.runs)
        if (r.shift != 0) {
            anyShift = true;
            break;
        }
    if (!anyShift) {
        auto *item = new QGraphicsTextItem(parent);
        item->document()->setDocumentMargin(0);
        item->setTextWidth(-1);
        QTextCursor cur(item->document());
        for (const LineRun &r : line.runs) {
            QTextCharFormat fmt;
            fmt.setForeground(r.color);
            fmt.setFont(r.font);
            cur.insertText(r.text, fmt);
        }
        item->setTextInteractionFlags(Qt::TextSelectableByMouse);
        item->setPos(left, baselineY - line.ascent); // baseline lands on baselineY
        return;
    }
    double penX = left;
    for (const LineRun &r : line.runs) {
        auto *g = new QGraphicsSimpleTextItem(r.text, parent);
        g->setFont(r.font);
        g->setBrush(r.color);
        const QFontMetricsF rfm(r.font);
        // Positive shift raises the baseline (superscript), negative lowers it.
        const double dy = (r.shift / 100000.0) * baseAscent;
        g->setPos(penX, baselineY - rfm.ascent() - dy);
        penX += rfm.horizontalAdvance(r.text);
    }
}

// Lay out and create the items for every v2 text box collected during parse.
// Boxes are independent; paragraphs within a box stack from data-top, then the
// whole block is vertically anchored (t/ctr/b) inside the text area.
void layoutV2Boxes(const QVector<V2Para> &paras) {
    QHash<int, QVector<int>> byBox; // box id -> indices into paras
    for (int i = 0; i < paras.size(); ++i)
        byBox[paras[i].box].push_back(i);

    for (auto it = byBox.begin(); it != byBox.end(); ++it) {
        QVector<int> idx = it.value();
        std::sort(idx.begin(), idx.end(),
                  [&](int a, int b) { return paras[a].para < paras[b].para; });

        // Box-level geometry comes from the first paragraph (oxide emits the same
        // text-area top/height/anchor on every paragraph of a box).
        const V2Para &first = paras[idx.front()];
        const double top = first.top;
        const double areaH = first.areaH;
        const QChar vanchor = first.vanchor;

        // Pre-wrap every paragraph and compute the total block height so the whole
        // block can be anchored before any item is placed.
        struct PP { QVector<LaidLine> lines; double pitch; double ascent0; double height0;
                    double spcBefore; double spcAfter; };
        QVector<PP> pp;
        double H = 0.0;
        for (int k = 0; k < idx.size(); ++k) {
            const V2Para &p = paras[idx[k]];
            const QFontMetricsF bfm(p.font);
            PP e;
            e.lines = wrapParagraph(p);
            e.ascent0 = bfm.ascent() * p.comp.ascent;
            e.height0 = bfm.height() * p.comp.line;
            e.pitch = p.lineFixed > 0.0 ? p.lineFixed : e.height0 * p.lineFactor;
            e.spcBefore = p.spcBefore;
            e.spcAfter = p.spcAfter;
            const int nl = e.lines.size();
            H += e.spcBefore + e.height0 + (nl - 1) * e.pitch + e.spcAfter;
            pp.push_back(e);
        }

        double y0 = top; // block top for vertical-anchor 't'
        if (vanchor == QLatin1Char('c'))
            y0 = top + (areaH - H) / 2.0;
        else if (vanchor == QLatin1Char('b'))
            y0 = top + (areaH - H);

        double cursor = y0;
        for (int k = 0; k < idx.size(); ++k) {
            const V2Para &p = paras[idx[k]];
            const PP &e = pp[k];
            cursor += e.spcBefore;
            const QFontMetricsF bfm(p.font);
            const double baseAscent = bfm.ascent();
            for (int li = 0; li < e.lines.size(); ++li) {
                const LaidLine &line = e.lines[li];
                const double baselineY = cursor + e.ascent0 + li * e.pitch;
                double left = p.x + p.marL;      // anchor=start (left)
                if (li == 0)
                    left += p.indent;            // first-line indent (hanging list)
                if (p.halign == Qt::AlignHCenter)
                    left = p.x - line.width / 2.0;
                else if (p.halign == Qt::AlignRight)
                    left = p.x - line.width;
                emitLine(line, left, baselineY, baseAscent, p.parent);
            }
            cursor += e.height0 + (e.lines.size() - 1) * e.pitch + e.spcAfter;
        }
    }
}

// B2a vertical text (data-vert="mongolianVert"/"eaVert"): glyphs stack top to
// bottom within a column; successive columns advance right to left. Each glyph is
// its own QGraphicsSimpleTextItem, so no horizontal QTextDocument wrapping is
// involved -- CJK and Latin alike stack upright (unrotated), matching PowerPoint's
// mongolianVert. Geometry consumed (aligned with oxide for the vert case): x/y are
// the text box top-left, data-w/data-h the box size in EMU. A column wraps when the
// next glyph would exceed the box bottom (data-h absent => one unbounded column).
// Per-run colours from #19 tspans carry through per glyph.
void addVerticalText(const QXmlStreamAttributes &attrs,
                     const QVector<TextSegment> &segments, const QFont &font,
                     const QColor &defColor, QGraphicsItem *page, QString *outText) {
    QString text;
    QVector<QColor> colors;
    QVector<QFont> fonts; // per-glyph font (honours a run's own B2b font-size)
    const QFontMetricsF baseFm(font);
    double maxH = baseFm.height(); // widest line height across runs -> column pitch
    for (const TextSegment &seg : segments) {
        const QColor c = seg.color.isValid() ? seg.color : defColor;
        QFont f = font;
        if (seg.fontSizeEmu > 0.0) {
            int px = qRound(seg.fontSizeEmu * S);
            f.setPixelSize(px < 1 ? 1 : px);
            maxH = qMax(maxH, QFontMetricsF(f).height());
        }
        for (const QChar &ch : seg.text) {
            text.append(ch);
            colors.append(c);
            fonts.append(f);
        }
    }
    if (text.trimmed().isEmpty())
        return;

    const double step = maxH; // vertical advance between stacked glyphs
    const double colW = maxH; // a column's horizontal extent (CJK ~ square)
    const double boxLeft = attrNum(attrs, "x") * S;
    const double boxTop = attrNum(attrs, "y") * S;
    const double boxW = attrNum(attrs, "data-w", -1.0) * S;
    const double boxH = attrNum(attrs, "data-h", -1.0) * S;
    const double rightEdge = boxW > 0.0 ? boxLeft + boxW : boxLeft + colW;

    // B6: build cells. A CJK/other glyph is its own upright cell; a maximal run of
    // Latin/digits (with interior spaces) becomes ONE horizontal cell rotated 90°
    // clockwise into the vertical flow -- so "PowerPoint" reads as a single tilted
    // word instead of a stuttered column of single letters. '\n' forces a column.
    struct VCell { bool brk; bool rotated; QString text; QFont font; QColor color; double h; };
    QVector<VCell> cells;
    for (int i = 0; i < text.size();) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\n')) {
            cells.push_back({true, false, QString(), font, defColor, 0.0});
            ++i;
            continue;
        }
        if (classOf(ch) == CClass::Alnum) {
            // Merge this Latin/digit token (plus interior spaces) into one cell.
            const QFont f = fonts.at(i);
            const QColor c = colors.at(i);
            QString run;
            int j = i;
            while (j < text.size()) {
                const QChar cj = text.at(j);
                if (cj == QLatin1Char('\n'))
                    break;
                const CClass cc = classOf(cj);
                if (cc == CClass::Alnum || (cc == CClass::Space && j + 1 < text.size() &&
                                            classOf(text.at(j + 1)) == CClass::Alnum)) {
                    run.append(cj);
                    ++j;
                } else {
                    break;
                }
            }
            const double h = QFontMetricsF(f).horizontalAdvance(run);
            cells.push_back({false, true, run, f, c, h});
            i = j;
            continue;
        }
        cells.push_back({false, false, QString(ch), fonts.at(i), colors.at(i), step});
        ++i;
    }

    double colX = rightEdge - colW; // left x of the current (rightmost) column
    double y = boxTop;
    int inCol = 0; // cells placed in the current column (>=1 before we may wrap)
    for (const VCell &cell : cells) {
        if (cell.brk) { // explicit break -> next column
            colX -= colW;
            y = boxTop;
            inCol = 0;
            continue;
        }
        if (boxH > 0.0 && inCol > 0 && y + cell.h > boxTop + boxH) {
            colX -= colW; // column full: advance leftward, restart at the top
            y = boxTop;
            inCol = 0;
        }
        auto *g = new QGraphicsSimpleTextItem(cell.text, page);
        g->setFont(cell.font);
        g->setBrush(cell.color);
        if (cell.rotated) {
            // Rotate +90° CW about the item's top-left: glyphs then advance
            // downward and the run's line-height extends leftward by fontH. Anchor
            // the strip's right edge so it is centred within the column's width.
            const double fontH = QFontMetricsF(cell.font).height();
            g->setTransformOriginPoint(0, 0);
            g->setRotation(90);
            g->setPos(colX + (colW + fontH) / 2.0, y);
        } else {
            const double adv = QFontMetricsF(cell.font).horizontalAdvance(cell.text);
            g->setPos(colX + (colW - adv) / 2.0, y); // centre glyph within its column
        }
        y += cell.h;
        ++inCol;
    }

    if (outText) {
        if (!outText->isEmpty())
            outText->append('\n');
        outText->append(text);
    }
}

void addText(const QXmlStreamAttributes &attrs, const QVector<TextSegment> &segments,
             QGraphicsItem *page, QString *outText, QVector<TextEntry> *entries,
             QVector<V2Para> *v2) {
    // Width/wrapping/anchor logic all runs on the full concatenated string; only the
    // per-run colours differ. outText and reflow also use this joined string.
    QString text;
    for (const TextSegment &seg : segments)
        text.append(seg.text);
    if (text.trimmed().isEmpty())
        return;
    const double x = attrNum(attrs, "x") * S;
    const double baseline = attrNum(attrs, "y") * S;
    const QFont font = buildTextFont(attrs);

    const QColor fill = parseColor(attrs.value(QLatin1String("fill")));
    const QColor defColor = fill.isValid() ? fill : QColor(Qt::black);

    // B2a: mongolianVert/eaVert lay glyphs out in columns (own path); vert270 reuses
    // the horizontal layout below then rotates the whole item -90 degrees.
    const QStringRef vert = attrs.value(QLatin1String("data-vert"));
    if (vert == QLatin1String("mongolianVert") || vert == QLatin1String("eaVert")) {
        addVerticalText(attrs, segments, font, defColor, page, outText);
        return;
    }

    // Contract v2: oxide emits box geometry (data-anchor / data-para) and NO baked
    // baseline; ttc owns the whole layout. Collect the paragraph here and defer to
    // layoutV2Boxes() once the entire box has been read (so paragraphs can stack +
    // the block can be vertically anchored). Old oxide never emits these, so the
    // legacy baseline path below is untouched -> zero regression. (data-box is the
    // grouping key; a v2 paragraph always carries one.)
    if (v2 && (attrs.hasAttribute(QLatin1String("data-anchor")) ||
               attrs.hasAttribute(QLatin1String("data-para")))) {
        V2Para p;
        p.parent = page;
        bool okBox = false;
        p.box = attrs.value(QLatin1String("data-box")).toInt(&okBox);
        if (!okBox)
            p.box = -1000000 - v2->size(); // ungrouped: give it a private box id
        p.para = qRound(attrNum(attrs, "data-para", 0.0));
        p.x = x;
        const QStringRef anc = attrs.value(QLatin1String("text-anchor"));
        p.halign = anc == QLatin1String("middle") ? Qt::AlignHCenter
                   : anc == QLatin1String("end")  ? Qt::AlignRight
                                                  : Qt::AlignLeft;
        p.top = attrNum(attrs, "data-top") * S;
        p.areaH = attrNum(attrs, "data-areah") * S;
        const QStringRef va = attrs.value(QLatin1String("data-anchor"));
        p.vanchor = va == QLatin1String("ctr")  ? QLatin1Char('c')
                    : va == QLatin1String("b")   ? QLatin1Char('b')
                                                 : QLatin1Char('t');
        p.dataW = attrNum(attrs, "data-w", -1.0) * S;
        p.lineFactor = attrs.hasAttribute(QLatin1String("data-linespacing"))
                           ? attrNum(attrs, "data-linespacing", 100.0) / 100.0
                           : 1.0;
        p.lineFixed = attrs.hasAttribute(QLatin1String("data-linespacing-pts"))
                          ? attrNum(attrs, "data-linespacing-pts") * S
                          : -1.0;
        p.spcBefore = attrNum(attrs, "data-spc-before") * S;
        p.spcAfter = attrNum(attrs, "data-spc-after") * S;
        p.marL = attrNum(attrs, "data-ml") * S;
        p.indent = attrNum(attrs, "data-indent") * S;
        p.font = font;
        p.defColor = defColor;
        p.comp = fontCompFor(primaryFamily(attrs));
        p.segs = segments;
        v2->push_back(p);
        if (outText) { // preserve copy-all text in document order
            if (!outText->isEmpty())
                outText->append('\n');
            outText->append(text);
        }
        return;
    }

    const QFontMetricsF fm(font);
    const QStringRef anchor = attrs.value(QLatin1String("text-anchor"));
    // SVG y is the baseline; a QGraphicsTextItem is anchored by its top-left, so the
    // item top puts the first line's baseline back on y (document margin is 0).
    const double top = baseline - fm.ascent();

    auto *item = new QGraphicsTextItem(page);
    item->document()->setDocumentMargin(0); // no stray inset before the glyphs
    item->setFont(font);
    item->setDefaultTextColor(defColor);
    // Fast path (zero regression): a single run with no colour or size of its own is
    // plain text. Otherwise (#19 inline mixed colour, B2b per-run font-size) insert
    // each run with its own foreground/size via a cursor -- runs without a colour or
    // size fall back to the <text> defaults.
    const bool plain = segments.size() == 1 &&
                       !segments.front().color.isValid() &&
                       segments.front().fontSizeEmu <= 0.0;
    if (plain) {
        item->setPlainText(text);
    } else {
        QTextCursor cur(item->document());
        for (const TextSegment &seg : segments) {
            QTextCharFormat fmt;
            fmt.setForeground(seg.color.isValid() ? seg.color : defColor);
            // B2b: a run's own font-size (EMU) overrides the paragraph size. The base
            // font is pixel-sized (EMU * S), so the run is pixel-sized the same way to
            // stay on one scale (bullet marker sized to buSzPct * body size).
            if (seg.fontSizeEmu > 0.0) {
                int px = qRound(seg.fontSizeEmu * S);
                fmt.setProperty(QTextFormat::FontPixelSize, px < 1 ? 1 : px);
            }
            cur.insertText(seg.text, fmt);
        }
    }
    item->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // #21 list line-spacing + hanging indent: paragraph-level data-* attributes map
    // onto the first (only) text block's QTextBlockFormat. marL/indent are EMU box
    // offsets scaled by S; line-spacing is a percent (proportional) or a fixed EMU
    // height. The anchor/x still place the box; these indent within it.
    const bool hasMl = attrs.hasAttribute(QLatin1String("data-ml"));
    const bool hasIndent = attrs.hasAttribute(QLatin1String("data-indent"));
    const bool hasLs = attrs.hasAttribute(QLatin1String("data-linespacing"));
    const bool hasLsPts = attrs.hasAttribute(QLatin1String("data-linespacing-pts"));
    if (hasMl || hasIndent || hasLs || hasLsPts) {
        QTextCursor cur(item->document());
        cur.select(QTextCursor::Document);
        QTextBlockFormat bf = cur.blockFormat();
        if (hasMl)
            bf.setLeftMargin(attrNum(attrs, "data-ml") * S);
        if (hasIndent)
            bf.setTextIndent(attrNum(attrs, "data-indent") * S); // negative == hanging
        if (hasLs)
            bf.setLineHeight(attrNum(attrs, "data-linespacing"),
                             QTextBlockFormat::ProportionalHeight);
        else if (hasLsPts)
            bf.setLineHeight(attrNum(attrs, "data-linespacing-pts") * S,
                             QTextBlockFormat::FixedHeight);
        cur.setBlockFormat(bf);
    }

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
        // B3b: wrap at Unicode line-break opportunities (WrapAtWordBoundary). CJK
        // ideographs each carry their own break opportunity (UAX#14), so CJK still
        // wraps per character; a run of digits/ASCII letters is one unbreakable word,
        // so "2006" no longer splits into "200"/"6" in a narrow box. (A pure-alnum run
        // wider than the box overflows rather than breaking mid-token, which is the
        // intended trade -- punctuation like "/" or "." in URLs still offers breaks.)
        opt.setWrapMode(QTextOption::WordWrap);
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

    // B2a vert270: the text runs bottom-to-top, so rotate the laid-out item 90
    // degrees counter-clockwise about its top-left anchor. mongolianVert/eaVert took
    // the column path above and returned before reaching here.
    if (vert == QLatin1String("vert270")) {
        item->setTransformOriginPoint(0, 0);
        item->setRotation(-90);
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
                QGraphicsTextItem *prev = items[i - 1];
                const double reserved = origTop[i] - origTop[i - 1]; // space oxide gave prev
                const double actual = prev->boundingRect().height();  // forces layout
                // B4b: only a paragraph that actually WRAPPED (more than one laid-out
                // line) may overrun the single-line slot oxide reserved. A single line
                // whose bounding box is inflated by a large proportional line-spacing
                // (data-linespacing="200") must NOT push the next paragraph down --
                // doing so shoved the last spAutoFit bullet (长春天成) further below
                // its box. Multi-line wraps behave exactly as before.
                int lineCount = 1;
                const QTextBlock blk = prev->document()->firstBlock();
                if (blk.isValid() && blk.layout() && blk.layout()->lineCount() > 0)
                    lineCount = blk.layout()->lineCount();
                if (lineCount > 1)
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
    QVector<TextEntry> textEntries; // for post-parse same-box paragraph reflow (v1)
    QVector<V2Para> v2Paras;        // contract-v2 paragraphs, laid out after parse
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
            addText(attrs, readTextSegments(xml), parent, outText, &textEntries, &v2Paras);
        // Unknown elements: ignored (graceful degradation).
    }

    if (xml.hasError() && !page) {
        // Structurally broken SVG with no usable page: let the caller degrade.
        return nullptr;
    }
    // Now that every paragraph's wrapped height is known, cascade same-box
    // paragraphs downward so wrapping never overlaps the next paragraph (v1 path).
    reflowTextBoxes(textEntries);
    // Contract-v2 boxes: lay out + create all items now that each box is complete.
    layoutV2Boxes(v2Paras);
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
