#include "SyncActionDelegate.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>

#include "SyncModel.h"

namespace {

// Matches SyncModel's palette.
const QColor kArrowColor(0x27, 0x7a, 0x46);
const QColor kConflictColor(0xd3, 0x8b, 0x1e);
const QColor kMutedColor(0x88, 0x88, 0x88);

QString glyphFor(SyncModel::Direction direction) {
    switch (direction) {
    case SyncModel::Direction::ToRight:
        return QStringLiteral("→");
    case SyncModel::Direction::ToLeft:
        return QStringLiteral("←");
    case SyncModel::Direction::Conflict:
        return QStringLiteral("≠");
    case SyncModel::Direction::Skip:
        return QStringLiteral("·");
    case SyncModel::Direction::None:
        break;
    }
    return QStringLiteral("=");
}

QColor colorFor(SyncModel::Direction direction) {
    switch (direction) {
    case SyncModel::Direction::ToRight:
    case SyncModel::Direction::ToLeft:
        return kArrowColor;
    case SyncModel::Direction::Conflict:
        return kConflictColor;
    case SyncModel::Direction::Skip:
    case SyncModel::Direction::None:
        break;
    }
    return kMutedColor;
}

} // namespace

SyncActionDelegate::SyncActionDelegate(SyncModel *model, QObject *parent)
    : QStyledItemDelegate(parent), m_model(model) {}

void SyncActionDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    // Draw the selection/background through the style, then the glyph on top, so
    // the cell keeps the theme's row highlighting.
    opt.text.clear();
    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    if (index.row() < 0 || index.row() >= m_model->rowCount())
        return;
    const SyncModel::Direction direction = m_model->directionAt(index.row());

    painter->save();
    QFont font = opt.font;
    font.setPointSizeF(font.pointSizeF() * 1.2);
    painter->setFont(font);
    painter->setPen(colorFor(direction));
    painter->drawText(opt.rect, Qt::AlignCenter, glyphFor(direction));
    painter->restore();
}

QSize SyncActionDelegate::sizeHint(const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setWidth(qMax(size.width(), 44));
    return size;
}

bool SyncActionDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                      const QStyleOptionViewItem &option,
                                      const QModelIndex &index) {
    if (event->type() != QEvent::MouseButtonRelease)
        return QStyledItemDelegate::editorEvent(event, model, option, index);

    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton || !option.rect.contains(mouse->pos()))
        return QStyledItemDelegate::editorEvent(event, model, option, index);

    m_model->cycleDirection(index.row());
    return true;
}
