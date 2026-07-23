#pragma once

#include <QPixmap>

class QIcon;

namespace ttc {

// Builds the pixmap shown under the cursor while dragging file-list items.
//   * A single item drags as its own icon.
//   * Several items drag as the first item's icon with faded offset copies
//     stacked behind it (a "pile of files" effect) and a count badge, so the
//     drag reads as a batch at a glance.
// `dpr` is the target device-pixel-ratio so the result stays crisp on HiDPI.
QPixmap makeDragPixmap(const QIcon &firstIcon, int count, qreal dpr = 1.0);

} // namespace ttc
