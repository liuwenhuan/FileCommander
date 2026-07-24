#pragma once

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <QStringList>

class QGraphicsItem;

// Parser for the office_oxide pptx-SVG subset into a QGraphicsScene item tree.
//
// office_oxide emits one standalone SVG per slide in EMU coordinate space (see
// convert_pptx_svg.rs). Rather than rasterize each slide with QSvgRenderer (text
// unselectable, blurry when zoomed), we translate the SVG into native graphics
// items so text stays selectable and every shape scales as vectors.
//
// The scene works in EMU/100 units (kSceneScale) rather than raw EMU: a title at
// 44 pt is 558800 EMU, and a QFont whose pixel size is that large overflows Qt's
// 26.6 fixed-point text layout (QFIXED_MAX) on a long line. Dividing by 100 keeps
// glyph metrics well inside that range while preserving sub-EMU precision, and the
// view's fit-to-width transform scales the whole scene to the pane afterwards.
namespace SlideScene {

// Scene unit == 1/100 of an EMU. Applied uniformly to every coordinate so shapes,
// images and text share one coordinate system.
constexpr double kSceneScale = 0.01;

// Parse one slide's SVG into a white "page" background rect (its child items are
// the slide's shapes and text). Local coordinates of the returned item are the
// slide's EMU space multiplied by kSceneScale, so the caller positions pages by
// setPos() and reads the page size from *outSizeScene (scene units).
//
// Text elements become selectable QGraphicsTextItem (Qt::TextSelectableByMouse);
// *outText, if non-null, receives the slide's concatenated text for the
// "copy all text" fallback. Unknown elements are skipped rather than fatal, and a
// structurally unparseable SVG yields nullptr so the caller can degrade.
QGraphicsItem *buildSlidePage(const QByteArray &svg, QSizeF *outSizeScene, QString *outText);

// Lightweight metadata-only parse: the slide's scene size (from the viewBox) and
// its concatenated text, WITHOUT building any graphics items or decoding embedded
// images. Cheap enough to run for every slide of a big deck up front, so the deck
// can lay out placeholders and populate copy-all text while only the on-screen
// slides pay the cost of a full buildSlidePage().
void parseSlideMeta(const QByteArray &svg, QSizeF *outSizeScene, QString *outText);

// Text-only, per-paragraph parse: each non-empty <text> element's inner text in
// document order, applying the same skip-empty rule buildSlidePage uses when it
// creates one QGraphicsTextItem per <text>. So the Nth string here corresponds to
// the Nth text item of the built slide -- used by in-page find to locate a match's
// item without decoding the slide's images.
QStringList parseSlideTexts(const QByteArray &svg);

// Drop the embedded-image decode cache. buildSlidePage memoises decoded, downscaled
// pixmaps by content hash so a repeated master background or a re-scrolled slide is
// decoded once; call this when switching away from a deck so the next file starts
// with an empty cache.
void clearImageCache();

} // namespace SlideScene
