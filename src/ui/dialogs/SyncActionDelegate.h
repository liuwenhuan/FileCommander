#pragma once

#include <QStyledItemDelegate>

class SyncModel;

// Paints the action column that sits between the two panes and turns it into
// the primary control of the dialog.
//
// Total Commander shows a read-only status glyph here and makes you change the
// copy direction from a separate toolbar. Making the cell itself clickable puts
// the control where the user is already looking: click the arrow on the row you
// want to change and it cycles through the directions that row actually
// supports. That is why this is a delegate with an editorEvent rather than a
// plain decoration role.
class SyncActionDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit SyncActionDelegate(SyncModel *model, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

protected:
    // Cycles the row's direction on a left click inside the action cell.
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

private:
    SyncModel *m_model;
};
