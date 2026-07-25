#pragma once

#include <QPoint>
#include <QRect>
#include <QSlider>

class QMouseEvent;

// A QSlider that jumps to wherever the groove is clicked, the way media player
// progress bars behave: the plain QSlider only pages towards the click (or
// nothing at all, depending on the style), which makes seeking a drag-only
// gesture. The click also arms the normal drag, so press-move-release keeps
// working as one continuous gesture.
//
// Only meant for progress/seek bars — a volume slider is better left with the
// stock behaviour, where a stray click can't blow the volume to maximum.
class SeekSlider : public QSlider {
    Q_OBJECT
public:
    explicit SeekSlider(Qt::Orientation orientation, QWidget *parent = nullptr);

    // Maps a click inside the groove to a slider value. Static and fed only
    // geometry so the mapping can be exercised without a live style; the
    // rects come from QStyle::subControlRect, which already accounts for the
    // handle size and mirrors itself under a right-to-left layout.
    static int valueForClick(const QRect &groove, const QRect &handle, const QPoint &pos,
                             Qt::Orientation orientation, int minimum, int maximum,
                             bool upsideDown);

protected:
    void mousePressEvent(QMouseEvent *event) override;
};
