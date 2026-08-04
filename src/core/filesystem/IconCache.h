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

    // The system's own icon for one REAL path, cached per path rather than per
    // type. Almost nothing wants this -- the whole point of the class is that
    // files share icons by extension -- but a drive does: Windows gives each
    // one an icon that depends on what it is (fixed, removable, optical, a
    // mapped network share) and often on an autorun.inf the vendor shipped, so
    // "the icon for a directory" is the wrong answer for it.
    //
    // Null when the platform has no such notion or the path does not exist, so
    // a caller can fall back to its own artwork without checking the platform.
    // Tinted like everything else, so it still follows the theme.
    QIcon systemIconForPath(const QString &path);

    // Applies the theme-owned tint and pixel grid to an arbitrary chrome icon.
    // With no active tint, returns the icon unchanged.
    QIcon themedIcon(const QIcon &icon) const;

    // Two tints, because the two kinds of icon answer to different questions.
    //
    // GLYPH tint (themedIcon): our own SVG chrome -- the drive, computer and
    // protocol glyphs, drawn in a mid grey that reads as dim on a dark
    // background. Every theme names a colour here so those glyphs sit at the
    // same weight as the text they label. They carry no colour of their own, so
    // recolouring them loses nothing.
    //
    // FILE-ICON tint (iconFor, systemIconForPath): the system's file-type and
    // drive icons. Only the CRT theme sets one, where collapsing everything to
    // a single phosphor hue IS the theme. Any other theme must leave these
    // alone: they are full-colour artwork, and tinting them turned every folder
    // in the light theme into a dark grey slab.
    //
    // Both live here, rather than in a delegate, because there is exactly one
    // place icons enter the app and because the result has to be cached -- the
    // recolour is a per-pixel pass and the file list asks for icons constantly.
    // Changing either clears the cache, so a theme switch takes effect at once.
    //
    // `blockPixels` is the quantisation grid for file icons, in image pixels.
    // No theme sets it: quantising a glyph made file types unidentifiable at
    // list-view sizes (see ThemeManager). Kept so the pass stays reachable.
    void setGlyphTint(const QColor &tint);
    void setFileIconTint(const QColor &tint, int blockPixels = 0);

private:
    IconCache();
    // Luminance-to-hue remap of every pixmap in `icon` to `tint`, alpha
    // preserved. Returns `icon` unchanged when `tint` is invalid.
    QIcon tinted(const QIcon &icon, const QColor &tint) const;

    QCache<QString, QIcon> m_cache;
    QColor m_glyphTint;    // invalid => hand our own SVG chrome through untouched
    QColor m_fileIconTint; // invalid => leave the system's artwork in its colours
    int m_blockPixels = 0; // icon quantisation grid; 0 => none
};
