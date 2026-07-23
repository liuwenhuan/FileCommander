#pragma once

#include <QPointer>
#include <QStyledItemDelegate>

class QAbstractItemView;

// Icon-grid delegate for a QListView (IconMode) over FileSystemModel: draws
// a centered thumbnail (from ThumbnailCache, falling back to the model's
// regular Qt::DecorationRole icon) with the file name wrapped to up to two
// lines beneath it. The parent ("..") entry gets a drawn "up" arrow instead
// of the usual folder/generic pixmap.
//
// Self-contained -- it does not touch FilePanel/FileListView/MainWindow. An
// integrator wires it up with something like:
//   auto *delegate = new ThumbnailDelegate(listView);
//   delegate->setIconSize(96);
//   delegate->setFontPointSize(9);
//   delegate->setView(listView);
//   listView->setItemDelegate(delegate);
class ThumbnailDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit ThumbnailDelegate(QObject *parent = nullptr);

    // Thumbnail/icon edge length in pixels; the cell grows to fit it plus
    // the name label. Defaults to a reasonable icon-view size.
    void setIconSize(int px);
    int iconSize() const { return m_iconSize; }

    // Point size for the name label drawn beneath each thumbnail.
    void setFontPointSize(int pt);
    int fontPointSize() const { return m_fontPointSize; }

    // The view this delegate is installed on. Only needed so the delegate
    // can repaint cells once a background-generated thumbnail lands
    // (ThumbnailCache::thumbnailReady fires well after paint() returned).
    void setView(QAbstractItemView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // The exact cell rect the delegate paints into (icon + up-to-two-line label
    // + margins) for a given base font. This is the single source of truth for
    // sizeHint(); FilePanel also uses it to size the icon view's grid so the
    // selection tile frames one cell precisely, without overlapping (or clipping
    // into) neighbouring rows. `baseFont` is the view font; the label point size
    // override (if any) is applied on top, matching paint().
    QSize cellSizeHint(const QFont &baseFont) const;

private slots:
    // Repaints the cell(s) for `path` if the view currently has a row
    // showing it, so a thumbnail that finished generating after paint()
    // already ran a fallback icon actually appears without user input.
    void onThumbnailReady(const QString &path);

private:
    // Resolves the absolute filesystem path backing `index`, or an empty
    // string if the model isn't a FileSystemModel (defensive -- this
    // delegate is only meant to be installed over one).
    static QString pathForIndex(const QModelIndex &index);

    int m_iconSize = 96;
    int m_fontPointSize = -1; // -1 == inherit the view's default font size
    QPointer<QAbstractItemView> m_view;
};
