#include "ThumbnailDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QIcon>
#include <QLineEdit>
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

    // The arrow takes the TEXT colour, not the highlight. It shares the grid
    // with file icons that follow the theme (Image Colours Follow Theme), and a
    // saturated accent among them is the one coloured thing on screen -- it
    // read as a leftover rather than as an affordance. The backdrop keeps the
    // highlight, faintly, so the cell still says "this one is special".
    const QColor ink = palette.color(QPalette::Normal, QPalette::WindowText);

    QColor backdrop = palette.color(QPalette::Highlight);
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
    painter->setBrush(ink);
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

qreal ThumbnailDelegate::devicePixelRatio() const {
    // The view's own ratio, not qApp's: with two displays at different scaling
    // qApp reports the maximum, which would over-generate on the 1x screen.
    // QWidget::devicePixelRatioF() follows the window between screens.
    const qreal dpr = m_view ? m_view->devicePixelRatioF() : qreal(1.0);
    return dpr > 0.0 ? dpr : qreal(1.0);
}

int ThumbnailDelegate::thumbnailPixelSize() const {
    return qMax(1, qRound(m_iconSize * devicePixelRatio()));
}

void ThumbnailDelegate::setFontPointSize(int pt) { m_fontPointSize = pt; }

void ThumbnailDelegate::setView(QAbstractItemView *view) { m_view = view; }

FileInfo ThumbnailDelegate::fileInfoForIndex(const QModelIndex &index) {
    if (!index.isValid())
        return {};
    // This delegate is only ever installed over a FileSystemModel (per the
    // class contract); the cast is defensive rather than load-bearing.
    if (const auto *model = qobject_cast<const FileSystemModel *>(index.model()))
        return model->fileInfoAt(index.row());
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

    // The keyboard cursor, drawn whether or not the tile is selected: arrow keys
    // move the current index without changing the selection (IconFileView::
    // keyPressEvent), so on an unselected tile this dashed frame is the only
    // feedback the key produced anything. Dashed, and inset inside the solid
    // selection stroke, so "current" and "selected" stay tellable apart.
    if (m_view && m_view->currentIndex().isValid() && m_view->currentIndex() == index) {
        const QRectF tile = QRectF(cell).adjusted(kTileInset + 2, kTileInset + 2,
                                                  -kTileInset - 2, -kTileInset - 2);
        QPen pen(opt.palette.color(QPalette::Highlight), 1, Qt::DashLine);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(pen);
        painter->drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), kTileRadius, kTileRadius);
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
        const FileInfo info = fileInfoForIndex(index);
        const QString path = info.path();
        QPixmap pixmap;
        // Thumbnails are requested in device pixels so they map 1:1 onto the
        // screen; `pixmapDpr` records how many device pixels of the result make
        // up one logical pixel, which is what the layout below works in. The
        // two sources disagree on this, so it is tracked per source rather than
        // read off the pixmap: a generated thumbnail is an untagged (dpr=1)
        // bitmap that happens to hold devicePx pixels, while QIcon::pixmap()
        // returns a properly tagged HiDPI pixmap.
        const int devicePx = thumbnailPixelSize();
        qreal pixmapDpr = devicePixelRatio();
        if (!path.isEmpty() && !info.isDir() && ThumbnailCache::canThumbnail(path)) {
            // A network tab's paths are meaningless to the local filesystem, so
            // those rows go down the fetch-then-decode route with the metadata
            // the listing already carries. Everything else (local files, and
            // archive entries extracted to a real path) stays on the local path.
            const QString connectionId = model ? model->connectionId() : QString();
            if (!connectionId.isEmpty()) {
                pixmap = ThumbnailCache::instance().remoteThumbnail(
                    model->providerPtr(), connectionId, path,
                    info.modified().toSecsSinceEpoch(), info.size(), devicePx);
            } else {
                pixmap = ThumbnailCache::instance().thumbnail(path, devicePx);
            }
        }

        if (pixmap.isNull()) {
            // No thumbnail available (unsupported type, or generation still in
            // flight) -- fall back to the model's regular per-type/folder icon.
            // QIcon::pixmap() takes a logical size and tags the result itself.
            const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            if (!icon.isNull()) {
                pixmap = icon.pixmap(QSize(m_iconSize, m_iconSize));
                pixmapDpr = pixmap.devicePixelRatio() > 0 ? pixmap.devicePixelRatio() : 1.0;
                // QIcon::pixmap() scales a bitmap DOWN to the size asked for but
                // never up, so a type whose icon tops out at 48px was drawn at
                // 48px -- a small badge marooned in a tile the user had zoomed
                // to 192. Most types register a 256px variant and never hit
                // this; the ones that do not are the "some icons don't follow
                // the zoom" the grid was reported for.
                //
                // Measured and grown in LOGICAL pixels, keeping the pixmap's own
                // dpr tag: the size asked for above is logical too, and QIcon
                // already did the HiDPI arithmetic to answer it. Re-doing that
                // here in device pixels would overflow the box by the scale
                // factor. KeepAspectRatio because a few icons are not square.
                const int longest = qRound(qMax(pixmap.width(), pixmap.height()) / pixmapDpr);
                if (longest > 0 && longest < m_iconSize) {
                    const int target = qRound(m_iconSize * pixmapDpr);
                    pixmap = pixmap.scaled(QSize(target, target), Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
                    pixmap.setDevicePixelRatio(pixmapDpr);
                }
            }
        }

        if (!pixmap.isNull()) {
            // Lay out in logical (device-independent) pixels: both sources hand
            // back pixmaps measured in *physical* pixels, so using width()/
            // height() directly made the icon render at dpr times its intended
            // size and spill outside the cell (and its selection frame).
            // Dividing collapses back to the logical box, and because the
            // pixmap now holds exactly as many pixels as that box covers on
            // screen, drawPixmap() blits it 1:1 instead of stretching it.
            const qreal dpr = pixmapDpr;
            const int w = qRound(pixmap.width() / dpr);
            const int h = qRound(pixmap.height() / dpr);
            // Center within iconRect regardless of the pixmap's own aspect
            // ratio (thumbnails preserve aspect ratio, so they're rarely square).
            const QRect target(iconRect.x() + (iconRect.width() - w) / 2,
                                iconRect.y() + (iconRect.height() - h) / 2, w, h);
            painter->drawPixmap(target, pixmap);
        }
    }

    const QFont font = labelFont(opt.font);
    painter->setFont(font);
    // The selection tile is only lightly tinted (the view background still
    // shows through), so the ordinary Text colour stays legible -- no need to
    // switch to HighlightedText, which would assume a fully-filled highlight.
    painter->setPen(opt.palette.color(QPalette::Normal, QPalette::Text));

    const QFontMetrics fm(font);
    const QString name = index.data(Qt::DisplayRole).toString();
    const int lineHeight = fm.height();
    const QRect textRect = labelRect(cell, font);
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

QFont ThumbnailDelegate::labelFont(const QFont &baseFont) const {
    QFont font = baseFont;
    if (m_fontPointSize > 0)
        font.setPointSize(m_fontPointSize);
    return font;
}

QRect ThumbnailDelegate::labelRect(const QRect &cell, const QFont &font) const {
    // Mirrors paint(): the icon box sits kMargin below the top, the label
    // starts kIconTextGap under it and is two lines tall.
    const int iconBottom = cell.top() + kMargin + m_iconSize - 1;
    return QRect(cell.left() + kTextPadding, iconBottom + kIconTextGap,
                 cell.width() - 2 * kTextPadding, 2 * QFontMetrics(font).height());
}

QWidget *ThumbnailDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                                         const QModelIndex &index) const {
    QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
    // The label is drawn at the delegate's own point size, which at a large
    // zoom is well above the view's. An editor left at the view font would
    // show the name in a noticeably smaller type than the row beside it.
    if (editor)
        editor->setFont(labelFont(option.font));
    if (auto *line = qobject_cast<QLineEdit *>(editor))
        line->setAlignment(Qt::AlignHCenter); // the label is centred; the editor should be too
    return editor;
}

void ThumbnailDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                                             const QModelIndex &index) const {
    Q_UNUSED(index);
    if (!editor)
        return;
    const QRect label = labelRect(option.rect, labelFont(option.font));
    // At least tall enough for the editor's own frame and text: two label lines
    // is normally more than that, but a small font with a chunky style frame is
    // not, and a clipped editor is the defect being fixed.
    const int height = qMax(label.height(), editor->sizeHint().height());
    // Kept inside the cell so a rename never paints over the neighbouring
    // tile's label -- growing the box is not worth making two rows unreadable.
    const int bottom = option.rect.bottom() - kTileInset;
    editor->setGeometry(QRect(label.left(), qMin(label.top(), bottom - height),
                              label.width(), height));
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
