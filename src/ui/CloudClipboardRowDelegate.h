#pragma once

#include <QHash>
#include <QPixmap>
#include <QStyledItemDelegate>

class CloudClipboardRowDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    enum DataRole {
        RecordIdRole = Qt::UserRole,
        KindRole,
        ContentRole,
        ImagePathRole,
        TimeRole,
        DeviceRole,
    };

    explicit CloudClipboardRowDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    QPixmap thumbnail(const QString &path) const;

    mutable QHash<QString, QPixmap> m_thumbnails;
};
