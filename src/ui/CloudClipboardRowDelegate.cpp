#include "CloudClipboardRowDelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QImageReader>
#include <QPainter>
#include <QStyle>
#include <QTextLayout>
#include <QTextOption>

namespace {
constexpr int kRowHeight = 44;
constexpr int kHorizontalMargin = 7;
constexpr int kVerticalMargin = 2;
constexpr int kMetadataWidth = 126;
constexpr int kColumnGap = 8;
constexpr int kThumbnailSize = kRowHeight - 2 * kVerticalMargin;

void drawTwoLines(QPainter *painter, const QRect &rect, const QString &text,
                  const QFont &font, const QColor &color) {
    painter->setFont(font);
    painter->setPen(color);

    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine first = layout.createLine();
    if (!first.isValid()) {
        layout.endLayout();
        return;
    }
    first.setLineWidth(rect.width());
    const int firstEnd = first.textStart() + first.textLength();
    const QFontMetrics metrics(font);
    const QString remainder = text.mid(firstEnd).trimmed();
    const int lineCount = remainder.isEmpty() ? 1 : 2;
    const int top = rect.top() + qMax(0, (rect.height() - lineCount * metrics.height()) / 2);
    painter->drawText(QRect(rect.left(), top, rect.width(), metrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, text.left(firstEnd));

    if (!remainder.isEmpty()) {
        painter->drawText(QRect(rect.left(), top + metrics.height(), rect.width(), metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(remainder, Qt::ElideRight, rect.width()));
    }
    layout.endLayout();
}
} // namespace

CloudClipboardRowDelegate::CloudClipboardRowDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

void CloudClipboardRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const {
    QStyleOptionViewItem styled(option);
    initStyleOption(&styled, index);
    styled.text.clear();
    styled.icon = QIcon();
    if (const QWidget *widget = styled.widget)
        widget->style()->drawControl(QStyle::CE_ItemViewItem, &styled, painter, widget);
    else
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &styled, painter);

    const bool selected = styled.state.testFlag(QStyle::State_Selected);
    const QPalette::ColorGroup group = selected ? QPalette::Active : QPalette::Normal;
    const QColor textColor = styled.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
    const QRect content = option.rect.adjusted(kHorizontalMargin, kVerticalMargin,
                                                -kHorizontalMargin, -kVerticalMargin);
    const QRect metadata(content.right() - kMetadataWidth + 1, content.top(), kMetadataWidth, content.height());
    const QRect left(content.left(), content.top(),
                     qMax(0, metadata.left() - content.left() - kColumnGap), content.height());

    const QString kind = index.data(KindRole).toString();
    if (kind == QLatin1String("image")) {
        const QRect box(left.left(), left.top() + (left.height() - kThumbnailSize) / 2,
                        kThumbnailSize, kThumbnailSize);
        const QPixmap image = thumbnail(index.data(ImagePathRole).toString());
        if (image.isNull()) {
            QColor placeholder = textColor;
            placeholder.setAlpha(110);
            painter->setPen(placeholder);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(box.adjusted(0, 0, -1, -1));
            painter->drawLine(box.topLeft(), box.bottomRight());
            painter->drawLine(box.topRight(), box.bottomLeft());
        } else {
            const QRect target(box.left() + (box.width() - image.width()) / 2,
                               box.top() + (box.height() - image.height()) / 2,
                               image.width(), image.height());
            painter->drawPixmap(target, image);
        }
    } else {
        drawTwoLines(painter, left, index.data(ContentRole).toString(), styled.font, textColor);
    }

    const QFont metadataFont = styled.font;
    const QFontMetrics metrics(metadataFont);
    painter->setFont(metadataFont);
    painter->setPen(textColor);
    const int metadataTop = metadata.top() +
                            qMax(0, (metadata.height() - 2 * metrics.height()) / 2);
    painter->drawText(QRect(metadata.left(), metadataTop, metadata.width(), metrics.height()),
                      Qt::AlignRight | Qt::AlignVCenter,
                      metrics.elidedText(index.data(TimeRole).toString(), Qt::ElideLeft, metadata.width()));
    painter->drawText(QRect(metadata.left(), metadataTop + metrics.height(), metadata.width(), metrics.height()),
                      Qt::AlignRight | Qt::AlignVCenter,
                      metrics.elidedText(index.data(DeviceRole).toString(), Qt::ElideLeft, metadata.width()));
}

QSize CloudClipboardRowDelegate::sizeHint(const QStyleOptionViewItem &option,
                                          const QModelIndex &index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(1, kRowHeight);
}

QPixmap CloudClipboardRowDelegate::thumbnail(const QString &path) const {
    if (path.isEmpty())
        return {};
    const auto cached = m_thumbnails.constFind(path);
    if (cached != m_thumbnails.cend())
        return *cached;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (sourceSize.isValid()) {
        QSize target = sourceSize;
        target.scale(kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio);
        reader.setScaledSize(target);
    }
    const QPixmap result = QPixmap::fromImage(reader.read());
    m_thumbnails.insert(path, result);
    return result;
}
