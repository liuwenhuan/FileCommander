#pragma once

#include <QCache>
#include <QColor>
#include <QIcon>
#include <QString>

class FileInfo;

// Per-type (not per-file) icon cache: file/directory icons are looked up
// by mime type / suffix rather than path, since many files share one
// icon. QCache is itself an LRU cache with a cost budget -- evicts the
// least-recently-used entries once the budget is exceeded, so this is a
// thin wrapper rather than a hand-rolled LRU.
class IconCache {
public:
    static IconCache &instance();

    QIcon iconFor(const FileInfo &info);

    // Applies the theme-owned tint and pixel grid to an arbitrary chrome icon.
    // With no active tint, returns the icon unchanged.
    QIcon themedIcon(const QIcon &icon) const;

    // Collapses every icon to one hue, scaled by the source pixel's brightness,
    // so the system icon theme's own colours (blue folders, red PDFs) do not
    // survive. Set by whoever owns theming; an invalid colour is the default and
    // means "hand icons through untouched".
    //
    // This lives here, rather than in a delegate, because there is exactly one
    // place icons enter the app and because the result has to be cached -- the
    // recolour is a per-pixel pass and the file list asks for icons constantly.
    // Changing the tint clears the cache, so a theme switch takes effect at once.
    // `blockPixels` is the quantisation grid for icons, in image pixels
    // (0 = none). Separate from fc::contentPixelBlock() because icons are
    // coarser -- a glyph survives a blocky raster, a photograph does not.
    void setTint(const QColor &tint, int blockPixels = 0);
    QColor tint() const { return m_tint; }

private:
    IconCache();
    // Luminance-to-hue remap of every pixmap in `icon`, alpha preserved.
    QIcon tinted(const QIcon &icon) const;

    QCache<QString, QIcon> m_cache;
    QColor m_tint;        // invalid => no recolouring
    int m_blockPixels = 0; // icon quantisation grid; 0 => none
};
