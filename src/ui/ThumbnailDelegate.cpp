#include "ThumbnailDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QStyle>

#include "FileSystemModel.h"
#include "ThumbnailCache.h"

namespace {
constexpr int kMargin = 6;      // padding above/below the icon and below the text
constexpr int kTextPadding = 4; // horizontal padding either side of the name label
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

    // Let the base style draw the selection/hover background and focus
    // rect, but with the built-in icon+text suppressed -- this delegate
    // lays those out itself (centered icon, centered name below).
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    opt.text.clear();
    opt.icon = QIcon();
    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const QRect cell = option.rect;
    const QRect iconRect(cell.left() + (cell.width() - m_iconSize) / 2, cell.top() + kMargin,
                          m_iconSize, m_iconSize);

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
        // Center within iconRect regardless of the pixmap's own aspect
        // ratio (thumbnails preserve aspect ratio, so they're rarely square).
        const QRect target(iconRect.x() + (iconRect.width() - pixmap.width()) / 2,
                            iconRect.y() + (iconRect.height() - pixmap.height()) / 2,
                            pixmap.width(), pixmap.height());
        painter->drawPixmap(target, pixmap);
    }

    QFont font = opt.font;
    if (m_fontPointSize > 0)
        font.setPointSize(m_fontPointSize);
    painter->setFont(font);
    painter->setPen(opt.palette.color(QPalette::Normal,
                                       (opt.state & QStyle::State_Selected)
                                           ? QPalette::HighlightedText
                                           : QPalette::Text));

    const QFontMetrics fm(font);
    const QString name = index.data(Qt::DisplayRole).toString();
    const QRect textRect(cell.left() + kTextPadding, iconRect.bottom() + kMargin,
                          cell.width() - 2 * kTextPadding, fm.height());
    const QString elided = fm.elidedText(name, Qt::ElideMiddle, textRect.width());
    painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, elided);

    painter->restore();
}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const {
    Q_UNUSED(index);
    QFont font = option.font;
    if (m_fontPointSize > 0)
        font.setPointSize(m_fontPointSize);
    const QFontMetrics fm(font);

    const int width = m_iconSize + 2 * kTextPadding;
    const int height = kMargin + m_iconSize + kMargin + fm.height() + kMargin;
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
