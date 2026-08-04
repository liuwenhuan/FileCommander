#pragma once

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QString>

// Recolouring every rendered surface to one phosphor hue, the way a monochrome
// CRT shows a colour signal: brightness survives, hue does not.
//
// The transform is the same everywhere it is used (file icons, thumbnails,
// image/PDF/slide previews, and video), which is the point -- a video frame and
// its own thumbnail must not come out different shades. Video goes through
// libavfilter rather than this code, so mpvFilterFor() below derives the filter
// string from the SAME numbers instead of restating them.
//
// Why not a translucent green overlay, which is the obvious first idea: an
// overlay can only add or multiply. Multiplying by pure green drives anything
// red to BLACK, because red has no green component -- but on a real green
// monitor a red object is mid-brightness green, since the tube is fed luma.
// Desaturate-then-colourise is what reproduces that, and it costs the same.
namespace fc {

// BT.601 luma weights -- what "brightness" means here, and what the mpv filter
// string is built from so the two paths cannot drift apart.
constexpr qreal kLumaR = 0.299;
constexpr qreal kLumaG = 0.587;
constexpr qreal kLumaB = 0.114;

// A pixel that had ink in the source never goes fully black: an outline drawn
// in near-black would otherwise vanish into the screen. Only applies to the
// still-image path -- video keeps a linear ramp, since a letterboxed black bar
// glowing faintly would look like a defect rather than a phosphor.
constexpr qreal kStillFloor = 0.20;

// Two content tints, because the two surfaces answer to two separate settings.
//
// THUMBNAIL tint: the icon grid. Set together with the file-icon tint (see
// IconCache) -- "the pictures in the file list match the theme" is one idea and
// one switch, whether a given cell shows a generated thumbnail or a type icon.
//
// PREVIEW tint: the preview pane's image, video, PDF and document pages. A
// separate switch because it is a different question: recolouring a wall of
// small thumbnails is decoration, while recolouring the picture someone opened
// to LOOK at changes what they are looking at. Plenty of people want the first
// and not the second.
//
// Invalid -- the default for both -- means "leave it alone", which is every
// non-CRT theme and the CRT theme with that switch off. Chrome glyphs are
// separate again (IconCache::setGlyphTint): they carry no colour of their own,
// so they follow the theme unconditionally.
QColor thumbnailTint();
void setThumbnailTint(const QColor &tint);
QColor previewTint();
void setPreviewTint(const QColor &tint);

// Size of one simulated pixel in the *image's own* pixels. 0 -- what every
// theme now sets -- disables quantisation: the coarse raster cost more
// legibility in thumbnails and file-type icons than it bought atmosphere, so
// the CRT theme carries its hue and its preview scanlines but not a grid.
// The pass below stays reachable for callers that want it explicitly.
int contentPixelBlock();
void setContentPixelBlock(int blockPixels);

// Collapses `image` onto a `block`-pixel grid: averaged down, then nearest-
// neighbour back up, so each cell is one flat square of the mean colour it
// covered. Averaging BEFORE the tint matters -- a cell then takes the mean
// brightness of what it covered, rather than the tint of one sampled pixel.
//
// Skipped when the image is too small to survive it: below about a dozen cells
// across, a file-type icon stops being identifiable, and an unrecognisable icon
// is a worse outcome than one that is a little too sharp.
void pixelate(QImage &image, int block);

// In place; alpha is preserved and fully transparent pixels are left untouched.
// `floor` lifts the darkest inked pixel off black (see kStillFloor).
void tintImage(QImage &image, const QColor &tint, qreal floor = kStillFloor);

// Recolours every inked pixel to exactly `tint`, keeping alpha as it is.
//
// The right mapping for a MONOCHROME glyph -- our own SVG chrome, every one of
// which is drawn in a single flat #888888. That grey carries no information, so
// running it through the luma ramp above only dims the result: 136 luma lands
// at 0.20 + 0.80*136/255 = 63% of the tint, which reads as a faded icon beside
// label text painted in the same colour at full strength. Anti-aliasing still
// softens the edges, because that lives in the alpha channel and is untouched.
void flattenToTint(QImage &image, const QColor &tint);

// Convenience wrapper: quantise (see pixelate) then colourise. Returns `src`
// unchanged when `tint` is invalid, so a caller can hand it the current
// thumbnailTint() without branching.
//
// `blockPixels` < 0 -- the default -- means "use contentPixelBlock()", which is
// what every content surface wants. Pass an explicit value to opt out (0, the
// app icon) or to use a different grid (icons, which are coarser).
QPixmap tintedPixmap(const QPixmap &src, const QColor &tint, int blockPixels = -1,
                     qreal floor = kStillFloor);

// The libavfilter chain that performs the same mapping on video frames, ready
// for mpv's "vf" option: one colorchannelmixer whose nine coefficients are the
// tint's channels times the luma weights above, preceded by the same
// average-down / nearest-up quantisation when `blockPixels` is set.
//
// `displayWidthPixels` is how wide the video is actually shown, in the image
// pixels the block is measured in; the frame is reduced to that width divided
// by the block, so a cell ends up the same size on screen as one in a
// thumbnail beside it -- regardless of whether the file is 480p or 4K. Pass 0
// to skip quantisation and tint only. Empty when `tint` is invalid.
QString mpvFilterFor(const QColor &tint, int blockPixels = 0, int displayWidthPixels = 0);

// Preview-only CRT raster treatment. It darkens every `period`th image row
// without reducing the image's detail and preserves source alpha.
constexpr int kScanlinePeriod = 2;
constexpr qreal kScanlineDarken = 0.28;
void applyScanlines(QImage &image, int period = kScanlinePeriod,
                    qreal darken = kScanlineDarken);

// Preview-specific phosphor transform: colourise without quantising, then add
// scanlines. This is deliberately separate from tintedPixmap(), whose callers
// include thumbnails and other surfaces outside the preview treatment.
QPixmap scanlinedPhosphorPixmap(const QPixmap &src, const QColor &tint,
                                int period = kScanlinePeriod,
                                qreal darken = kScanlineDarken);

// Legacy video-preview entry point retained for QuickView compatibility. Video
// deliberately applies only the low-cost phosphor tint: `period` and `darken`
// are ignored so the filter never introduces geq/mod(Y) per-pixel scanlines.
// Static QImage previews continue to receive scanlines via
// scanlinedPhosphorPixmap(). Empty when `tint` is invalid.
QString mpvScanlinedPhosphorFilter(const QColor &tint,
                                   int period = kScanlinePeriod,
                                   qreal darken = kScanlineDarken);

} // namespace fc
