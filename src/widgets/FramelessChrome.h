#pragma once

#include <QBrush>
#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <Qt>

class QPainter;

// The self-drawn window frame -- translucent drop shadow, rounded corners, and
// the band around the content that the WM resizes from. Two hosts wear it:
// FramelessDialog (a QDialog) and FramelessWindow (a top-level QWidget, for the
// F3 viewer and the F4 editor). It lives here rather than in either of them so
// the two cannot drift apart.
//
// MainWindow renders its frame with renderFrameTile() below but keeps its own
// copy of the constants and of the 9-patch blit, because it also maintains an
// XShape mask and an opaque-region hint that only a QMainWindow needs; those
// values are kept in sync with the ones below by hand.
namespace ttc::chrome {

constexpr int kShadowMargin = 16; // translucent margin: drop shadow + resize band
constexpr int kCornerRadius = 8;  // rounded window-corner radius
constexpr int kResizeGrab = 8;    // edge-resize grab band, straddling the content edge

// Edges whose grab band (straddling the visible content rectangle's edge)
// contains p. `content` is the window rect inset by the shadow margin.
Qt::Edges edgesAt(const QRect &content, const QPoint &p);

Qt::CursorShape cursorForEdges(Qt::Edges edges);

// Renders the shadow + rounded frame ONCE at the smallest representative size.
// blitFrame() then stretches it 9-patch style, which is what keeps an
// interactive resize from re-rasterizing 17 anti-aliased rounded rects over the
// whole window on every repaint.
QPixmap renderFrameTile(const QColor &background, const QPixmap &tile);

// Draws `frameTile` over a widget of `size`: four corners at 1:1, edges
// stretched along one axis, `centre` filling the middle.
void blitFrame(QPainter &painter, const QSize &size, const QPixmap &frameTile,
               const QBrush &centre);

} // namespace ttc::chrome
