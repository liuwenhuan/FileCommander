#pragma once

#include <QColor>
#include <QPixmap>

class QIcon;

namespace ttc {

// Builds the pixmap shown under the cursor while dragging file-list items.
//   * A single item drags as its own icon.
//   * Several items drag as the first item's icon with faded offset copies
//     stacked behind it (a "pile of files" effect) and a count badge, so the
//     drag reads as a batch at a glance.
// `dpr` is the target device-pixel-ratio so the result stays crisp on HiDPI.
// `accent`/`accentText` colour the count badge -- the caller passes its own
// palette's highlight pair, so the badge follows the theme instead of being a
// blue disc in a green or grey window. Invalid colours keep the former blue.
QPixmap makeDragPixmap(const QIcon &firstIcon, int count, qreal dpr = 1.0,
                       const QColor &accent = QColor(), const QColor &accentText = QColor());

} // namespace ttc
