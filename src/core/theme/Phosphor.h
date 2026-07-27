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

// Edge of one simulated screen pixel, in LOGICAL pixels. Logical, not device,
// so the visible chunk keeps the same physical size on a HiDPI display -- the
// same reason the scanline tiles are laid out in logical pixels.
//
// There are two values, not one, because the surfaces differ in what they can
// afford to lose:
//
//   * An ICON is a glyph. It carries one idea -- "folder", "video file" -- and
//     a coarse raster states that idea perfectly well. 4 makes a 64-pixel icon
//     16x16, which is unmistakably a raster and still obviously a folder.
//   * CONTENT is a photograph, a video frame or a document page, and it is
//     there to be looked at. The same 4 turned slide text into mush and left
//     thumbnails unreadable, so content gets half that.
//
// The app icon is a third case and takes neither: it is drawn about 20 pixels
// wide in the title bar, where any quantisation at all costs more legibility
// than it buys atmosphere. See ttc::appIcon().
constexpr int kIconBlockLogical = 4;
constexpr int kContentBlockLogical = 2;

// The tint applied to *content* (thumbnails, previews, video). Invalid -- the
// default -- means "leave content alone", which is both the non-CRT themes and
// the CRT theme with the follow-images toggle off. Chrome icons are tinted
// separately via IconCache::setTint(), because they follow the theme
// unconditionally while content follows the toggle.
QColor contentTint();
void setContentTint(const QColor &tint);

// Size of one simulated pixel in the *image's own* pixels, i.e.
// kContentBlockLogical scaled by the display's device pixel ratio. 0 disables
// quantisation. Set alongside the tint by whoever owns theming.
//
// This is the CONTENT block. Icons do not read it -- IconCache passes its own
// (see kIconBlockLogical), because it is tinted whenever the theme is CRT while
// content follows the separate images toggle.
//
// It is a single global rather than a per-surface value because the thumbnail
// cache stores bitmaps whose content is in device pixels but which are marked
// dpr=1, so there is nothing to read it back from at the point of use. The
// consequence is a real (small) limitation: on a multi-monitor setup with
// different scale factors, the block is whatever the primary screen implies,
// so chunks on the secondary screen are off by that ratio.
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

// Convenience wrapper: quantise (see pixelate) then colourise. Returns `src`
// unchanged when `tint` is invalid, so a caller can hand it the current
// contentTint() without branching.
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

// libavfilter equivalent for video preview: colourise then attenuate every
// alternate row, with no resize or sampling stage. Empty when `tint` is invalid.
QString mpvScanlinedPhosphorFilter(const QColor &tint,
                                   int period = kScanlinePeriod,
                                   qreal darken = kScanlineDarken);

} // namespace fc
