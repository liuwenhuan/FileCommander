#include "ThumbnailDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QStyle>
#include <QTextLayout>
#include <QTextOption>

#include "FileSystemModel.h"
#include "ThumbnailCache.h"

namespace {
constexpr int kMargin = 9;       // padding between the cell edge and the icon (all sides)
constexpr int kTextPadding = 4;  // horizontal padding either side of the name label
constexpr int kIconTextGap = 10; // clear gap between the thumbnail and its name
constexpr int kTileInset = 3;    // inset of the selection tile from the cell edge
constexpr int kTileRadius = 6;   // corner radius of the selection tile

// Draws a bold "return to parent" up-arrow (arrowhead over a stem) centered
// in `iconRect`, on a faint tinted rounded backdrop so the affordance reads
// clearly even for an unselected row. Colours derive from the palette's
// highlight so the arrow adapts automatically to light/dark themes.
void drawParentArrow(QPainter *painter, const QRect &iconRect, const QPalette &palette) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QColor accent = palette.color(QPalette::Highlight);

    QColor backdrop = accent;
    backdrop.setAlpha(36);
    painter->setPen(Qt::NoPen);
    painter->setBrush(backdrop);
    painter->drawRoundedRect(QRectF(iconRect), iconRect.width() * 0.22, iconRect.height() * 0.22);

    // The arrow itself occupies the central ~55% of iconRect, leaving room
    // for the backdrop to show as a clear border around it.
    const qreal side = iconRect.width() * 0.55;
    const QRectF square(iconRect.center().x() - side / 2.0, iconRect.center().y() - side / 2.0, side, side);

    // Classic 7-point "up arrow" outline (broad arrowhead over a centered
    // stem), defined in the square's local 0..1 space then scaled into it.
    const QPointF unitPoints[7] = {
        {0.50, 0.00}, {0.90, 0.42}, {0.68, 0.42}, {0.68, 1.00},
        {0.32, 1.00}, {0.32, 0.42}, {0.10, 0.42},
    };
    QPolygonF arrow;
    for (const QPointF &p : unitPoints)
        arrow << QPointF(square.left() + p.x() * square.width(), square.top() + p.y() * square.height());

    painter->setPen(Qt::NoPen);
    painter->setBrush(accent);
    painter->drawPolygon(arrow);

    painter->restore();
}

// Splits `name` into at most two display lines that fit within `width`:
// the first line breaks at a word boundary where possible (falling back to
// a mid-word break for long unbroken names, since WrapAtWordBoundaryOrAnywhere
// forces a break rather than overflowing); the second line holds whatever
// remains, elided with "..." if it still doesn't fit.
QPair<QString, QString> wrapNameToTwoLines(const QString &name, const QFont &font, const QFontMetrics &fm, int width) {
    QTextLayout layout(name, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return {name, QString()};
    }
    line.setLineWidth(width);
    const int firstLineEnd = line.textStart() + line.textLength();
    layout.endLayout();

    const QString firstLine = name.left(firstLineEnd);
    // A word-boundary break can leave a leading space on the remainder --
    // trim it so the second line doesn't start with a visible gap.
    const QString remainder = name.mid(firstLineEnd).trimmed();
    if (remainder.isEmpty())
        return {firstLine, QString()};

    const QString secondLine = fm.elidedText(remainder, Qt::ElideRight, width);
    return {firstLine, secondLine};
}
} // namespace

ThumbnailDelegate::ThumbnailDelegate(QObject *parent) : QStyledItemDelegate(parent) {
    // Connected once, unconditionally: a background thumbnail can finish
    // generating long after paint() ran (and fell back to the plain file
    // icon), so any time one lands we need to trigger a repaint. Checking
    // m_view for null inside the slot means this is harmless before
    // setView() is called.
    connect(&ThumbnailCache::instance(), &ThumbnailCache::thumbnailReady, this,
            &ThumbnailDelegate::onThumbnailReady);
}

void ThumbnailDelegate::setIconSize(int px) { m_iconSize = px > 0 ? px : m_iconSize; }

void ThumbnailDelegate::setFontPointSize(int pt) { m_fontPointSize = pt; }

void ThumbnailDelegate::setView(QAbstractItemView *view) { m_view = view; }

QString ThumbnailDelegate::pathForIndex(const QModelIndex &index) {
    if (!index.isValid())
        return {};
    // This delegate is only ever installed over a FileSystemModel (per the
    // class contract); the cast is defensive rather than load-bearing.
    if (const auto *model = qobject_cast<const FileSystemModel *>(index.model()))
        return model->fileInfoAt(index.row()).path();
    return {};
}

void ThumbnailDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const {
    painter->save();

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    // Draw the selection/hover state ourselves rather than deferring to the
    // platform style (which, in icon mode, only drew a thin highlight sliver by
    // the name -- easy to miss). A selected thumbnail gets a rounded, tinted
    // tile with a coloured frame around the whole icon+label; hover gets a
    // fainter fill. This is the obvious "this one is selected" cue.
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const QRect cell = option.rect;
    if (selected || hover) {
        const QColor highlight = opt.palette.color(QPalette::Highlight);
        QColor fill = highlight;
        fill.setAlpha(selected ? 60 : 28);
        const QRectF tile = QRectF(cell).adjusted(kTileInset, kTileInset, -kTileInset, -kTileInset);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(tile, kTileRadius, kTileRadius);
        if (selected) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(highlight, 1));
            // Half-pixel inset keeps the 1px stroke crisp on the tile edge.
            painter->drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), kTileRadius, kTileRadius);
        }
        painter->setRenderHint(QPainter::Antialiasing, false);
    }

    const QRect iconRect(cell.left() + (cell.width() - m_iconSize) / 2, cell.top() + kMargin,
                          m_iconSize, m_iconSize);

    // The parent (".." ) entry doesn't back a real file/thumbnail -- draw an
    // unmistakable "go up" arrow instead of the usual folder/generic pixmap.
    const auto *model = qobject_cast<const FileSystemModel *>(index.model());
    const bool isParentEntry = model && model->isParentEntry(index.row());

    if (isParentEntry) {
        drawParentArrow(painter, iconRect, opt.palette);
    } else {
        const QString path = pathForIndex(index);
        QPixmap pixmap;
        if (!path.isEmpty() && ThumbnailCache::canThumbnail(path))
            pixmap = ThumbnailCache::instance().thumbnail(path, m_iconSize);

        if (pixmap.isNull()) {
            // No thumbnail available (unsupported type, or generation still in
            // flight) -- fall back to the model's regular per-type/folder icon.
            const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            if (!icon.isNull())
                pixmap = icon.pixmap(QSize(m_iconSize, m_iconSize));
        }

        if (!pixmap.isNull()) {
            // Lay out in logical (device-independent) pixels: the fallback
            // QIcon::pixmap() returns a HiDPI-aware pixmap whose width()/height()
            // are in *physical* pixels (2x on a scaled display), so using them
            // directly made the icon render at double size and spill outside the
            // cell (and its selection frame). Dividing by the device pixel ratio
            // is a no-op for the dpr=1 thumbnail pixmaps.
            const qreal dpr = pixmap.devicePixelRatio() > 0 ? pixmap.devicePixelRatio() : 1.0;
            const int w = qRound(pixmap.width() / dpr);
            const int h = qRound(pixmap.height() / dpr);
            // Center within iconRect regardless of the pixmap's own aspect
            // ratio (thumbnails preserve aspect ratio, so they're rarely square).
            const QRect target(iconRect.x() + (iconRect.width() - w) / 2,
                                iconRect.y() + (iconRect.height() - h) / 2, w, h);
            painter->drawPixmap(target, pixmap);
        }
    }

    QFont font = opt.font;
    if (m_fontPointSize > 0)
        font.setPointSize(m_fontPointSize);
    painter->setFont(font);
    // The selection tile is only lightly tinted (the view background still
    // shows through), so the ordinary Text colour stays legible -- no need to
    // switch to HighlightedText, which would assume a fully-filled highlight.
    painter->setPen(opt.palette.color(QPalette::Normal, QPalette::Text));

    const QFontMetrics fm(font);
    const QString name = index.data(Qt::DisplayRole).toString();
    const int lineHeight = fm.height();
    const QRect textRect(cell.left() + kTextPadding, iconRect.bottom() + kIconTextGap,
                          cell.width() - 2 * kTextPadding, 2 * lineHeight);
    const QPair<QString, QString> lines = wrapNameToTwoLines(name, font, fm, textRect.width());

    const QRect firstLineRect(textRect.left(), textRect.top(), textRect.width(), lineHeight);
    painter->drawText(firstLineRect, Qt::AlignHCenter | Qt::AlignTop, lines.first);
    if (!lines.second.isEmpty()) {
        const QRect secondLineRect(textRect.left(), textRect.top() + lineHeight, textRect.width(), lineHeight);
        painter->drawText(secondLineRect, Qt::AlignHCenter | Qt::AlignTop, lines.second);
    }

    painter->restore();
}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const {
    Q_UNUSED(index);
    return cellSizeHint(option.font);
}

QSize ThumbnailDelegate::cellSizeHint(const QFont &baseFont) const {
    QFont font = baseFont;
    if (m_fontPointSize > 0)
        font.setPointSize(m_fontPointSize);
    const QFontMetrics fm(font);

    // kMargin on every side so the selection tile has clear breathing room
    // around the icon rather than hugging it (the old width used only the
    // narrower text padding, cramping the frame horizontally).
    const int width = m_iconSize + 2 * kMargin;
    // Reserve two lines of label height -- the name now wraps to up to two
    // lines instead of eliding to one (see wrapNameToTwoLines()/paint()).
    const int height = kMargin + m_iconSize + kIconTextGap + 2 * fm.height() + kMargin;
    return QSize(width, height);
}

void ThumbnailDelegate::onThumbnailReady(const QString &path) {
    Q_UNUSED(path);
    // Simplest correct option: repaint the whole viewport rather than
    // hunting for the exact row(s) showing `path` (a file can appear at
    // most once per panel anyway, and thumbnail generation is already
    // throttled by the bounded worker pool, so this isn't a hot path).
    if (m_view)
        m_view->viewport()->update();
}
