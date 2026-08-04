#pragma once

#include <QPointer>
#include <QStyledItemDelegate>

#include "FileInfo.h"

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

    // Thumbnail/icon edge length in *logical* (device-independent) pixels; the
    // cell grows to fit it plus the name label. Defaults to a reasonable
    // icon-view size. This is the layout size -- see thumbnailPixelSize() for
    // the size a thumbnail must actually be generated at.
    void setIconSize(int px);
    int iconSize() const { return m_iconSize; }

    // The edge length, in *device* pixels, that a thumbnail for this delegate
    // must be generated at: iconSize() scaled by the display's device pixel
    // ratio. On a 1.5x display a 192px icon box covers 288 device pixels, so a
    // 192px bitmap would be stretched by half again -- which is exactly the
    // blur that grows worse at every zoom step. Anyone requesting a thumbnail
    // on this delegate's behalf (FilePanel's network prefetch sweep) must ask
    // for this size, or it fills the cache under a key paint() never reads.
    int thumbnailPixelSize() const;

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

    // Inline rename. Both exist for one reason: the cell is laid out by this
    // delegate, so only this delegate knows where the name sits and how big it
    // is drawn. Left to QStyledItemDelegate, the editor is placed by the style's
    // SE_ItemViewItemText -- computed from the view's iconSize, which this
    // delegate does not use -- so it lands at a fixed small size wherever the
    // zoom happens to have put the label, showing a few characters of the name.
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override;

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
    // The listing entry backing `index`, or a default-constructed FileInfo if
    // the model isn't a FileSystemModel (defensive -- this delegate is only
    // meant to be installed over one). The whole entry rather than just its
    // path, because a remote thumbnail is keyed on the listing's own
    // size/mtime: there is no local file to stat for them.
    static FileInfo fileInfoForIndex(const QModelIndex &index);

    // The view font with this delegate's label point size applied, and the
    // rect the name is drawn into within `cell`. Shared by paint() and the
    // editor placement so the two cannot drift apart -- which is exactly how
    // the editor came to sit somewhere the label does not.
    QFont labelFont(const QFont &baseFont) const;
    QRect labelRect(const QRect &cell, const QFont &labelFont) const;

    // Device pixel ratio of the screen this delegate's view is currently on
    // (1.0 with no view yet). Read fresh on every use rather than cached: a
    // window dragged between displays of different scaling changes it at
    // runtime, and the ratio is what decides both the pixel size requested and
    // the logical size drawn.
    qreal devicePixelRatio() const;

    int m_iconSize = 96;
    int m_fontPointSize = -1; // -1 == inherit the view's default font size
    QPointer<QAbstractItemView> m_view;
};
