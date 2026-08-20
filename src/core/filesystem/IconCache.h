#pragma once

#include <QCache>
#include <QColor>
#include <QMutex>
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
    // Cache-only: answers if this drive's icon has already been fetched, and
    // otherwise returns null WITHOUT asking the shell. It is called from
    // FileSystemModel::data(), i.e. while painting, and the shell query below
    // talks to the volume itself -- a disconnected mapped drive or a spun-down
    // disk can hold that up for seconds, which is not something a paint can
    // afford. A null answer just means the caller's own artwork is used.
    QIcon systemIconForPath(const QString &path) const;

    // Does the shell query and caches it. Safe to call from a worker; the
    // caller repaints once it returns.
    void warmSystemIconForPath(const QString &path);

    // Whether warmSystemIconForPath is implemented on this platform at all.
    //
    // It is a shell query and exists only on Windows; elsewhere it returns
    // without doing anything, so systemIconForPath can never become non-null no
    // matter how long a caller waits. Answering that question here, beside the
    // implementation, is what keeps a caller from having to guess -- a test
    // that waited for an icon that was never coming is what prompted it.
    static bool hasSystemIconLookup();

    // One of our own SVG glyphs (":/icons/dev-smb.svg"), recoloured to the
    // theme's glyph colour at FULL strength and cached.
    //
    // Separate from themedIcon() because it can assume what that one cannot:
    // every glyph in resources/icons is drawn in a single flat #888888, so the
    // grey carries no information and running it through the luma ramp only
    // dims the icon -- measured at 63% of the tint, which reads as faded beside
    // label text painted in the same colour at full strength.
    QIcon glyphIcon(const QString &resourcePath);

    // Windows' jumbo image list hands back a 256x256 slot for every icon, even one
    // that never shipped a 256px variant, and pads the smaller bitmap out with
    // transparency rather than scaling it. The icon then CLAIMS a size it does not
    // have, and everything downstream believes it.
    //
    // Cropping to the ink is what makes the claim true again, and it has to be the
    // ink's own box -- an earlier version rounded up to a coarse rung (32/48/64/128)
    // and kept the top-left anchoring, which merely traded one lie for a smaller
    // one: measured in the running app, a .rar came back as a 128 canvas holding
    // 77x60 of ink at (3,12). On a 133% display that 128 is exactly what
    // QIcon::pixmap() is asked for at a 96px icon box, so the thumbnail delegate saw
    // a pixmap of precisely the right size and left it alone -- a small badge in a
    // large tile, and no test at 100% could reproduce it.
    //
    // Left alone when the ink already fills most of the canvas (.zip's genuine 256
    // measured 227x176), so a real icon of this size keeps its own margins.
    //
    // Public only so a test can drive it: which composition the shell hands
    // back depends on the display's scale factor, so a test that goes through
    // the real icon cannot reproduce the case that matters on every machine.
    static QImage cropPaddedIcon(const QImage &image, int size);

    // Applies the theme-owned tint and pixel grid to an arbitrary chrome icon --
    // one whose colours may mean something, such as a platform standard icon.
    // Artwork that turns out to be monochrome takes glyphIcon()'s full-strength
    // recolour instead of the luma ramp, for the reason given there: the
    // platform's media glyphs are flat near-black, which the ramp reads as dark
    // rather than as flat and leaves at its floor.
    // With no active tint, returns the icon unchanged.
    QIcon themedIcon(const QIcon &icon) const;
    // The same treatment against a colour the caller names, for artwork whose
    // right tint is not the glyph one. A FILLED mark recoloured towards
    // #404040 -- the glyph colour on a light background -- comes out a dark
    // slab, which is why file icons have their own bright tint and why the
    // message-box marks take it too.
    QIcon themedIcon(const QIcon &icon, const QColor &tint) const;

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
    //
    // `selectedTint` is the same colour question asked for a SELECTED row,
    // where the theme's selection colour is the fill behind the glyph: the CRT
    // theme selects by inverting, so a phosphor glyph on a phosphor fill
    // vanishes. Glyphs are built with a QIcon::Selected variant in that colour,
    // which is what a stock QStyledItemDelegate already asks for. Invalid =>
    // no such variant, and a selected glyph keeps the ordinary colour.
    void setGlyphTint(const QColor &tint, const QColor &selectedTint = QColor());
    void setFileIconTint(const QColor &tint, int blockPixels = 0);
    // Which of the two is in force, for whoever needs to reason about it --
    // an invalid colour means that kind of icon is left in its own colours.
    QColor glyphTint() const { return m_glyphTint; }
    QColor glyphSelectedTint() const { return m_glyphSelectedTint; }
    QColor fileIconTint() const { return m_fileIconTint; }

private:
    IconCache();
    // Luminance-to-hue remap of every pixmap in `icon` to `tint`, alpha
    // preserved. Returns `icon` unchanged when `tint` is invalid.
    // `flattenMonochrome` recolours monochrome artwork at full strength rather
    // than through the ramp; the file icons it is off for are full-colour by
    // nature, and a greyscale one among them is shading that has to survive.
    QIcon tinted(const QIcon &icon, const QColor &tint,
                 bool flattenMonochrome = false) const;

    // Guards the cache and the tints. The cache is filled from a worker (see
    // warmSystemIconForPath) as well as from the GUI thread.
    mutable QMutex m_mutex;
    QCache<QString, QIcon> m_cache;
    QColor m_glyphTint;    // invalid => hand our own SVG chrome through untouched
    QColor m_glyphSelectedTint; // invalid => no QIcon::Selected variant is built
    QColor m_fileIconTint; // invalid => leave the system's artwork in its colours
    int m_blockPixels = 0; // icon quantisation grid; 0 => none
};
