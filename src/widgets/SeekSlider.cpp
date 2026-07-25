#include "SeekSlider.h"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

SeekSlider::SeekSlider(Qt::Orientation orientation, QWidget *parent)
    : QSlider(orientation, parent) {}

int SeekSlider::valueForClick(const QRect &groove, const QRect &handle, const QPoint &pos,
                              Qt::Orientation orientation, int minimum, int maximum,
                              bool upsideDown) {
    // The click marks where the handle's centre should land, and the handle can
    // only travel (groove - handle) pixels, so both the offset and the span are
    // measured a handle short. Doing it any other way (pos / width * maximum)
    // drifts by half a handle at each end.
    const bool horizontal = orientation == Qt::Horizontal;
    const int span = horizontal ? groove.width() - handle.width()
                                : groove.height() - handle.height();
    const int offset = horizontal ? pos.x() - groove.x() - handle.width() / 2
                                  : pos.y() - groove.y() - handle.height() / 2;
    return QStyle::sliderValueFromPosition(minimum, maximum, offset, span, upsideDown);
}

void SeekSlider::mousePressEvent(QMouseEvent *event) {
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

    // Grabbing the handle, chording with another button, or an empty range is
    // the stock slider's business — only a plain left click on the groove is
    // reinterpreted as a jump.
    if (event->button() != Qt::LeftButton || event->buttons() != event->button() ||
        minimum() == maximum() || handle.contains(event->pos())) {
        QSlider::mousePressEvent(event);
        return;
    }

    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    const int target = valueForClick(groove, handle, event->pos(), orientation(), minimum(),
                                     maximum(), opt.upsideDown);

    // Feed QSlider a press on the handle's current centre before jumping: that
    // is what puts it into its normal drag state (sliderPressed now, then
    // sliderMoved / sliderReleased as usual), so the click can be held and
    // dragged onwards in one gesture. Pressing at the centre also leaves the
    // drag offset at half a handle, so the handle tracks the cursor from here.
    // Arming the drag first means a listener that freezes position updates on
    // sliderPressed is already armed when the value moves.
    const QPoint centre = handle.center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(centre), mapToGlobal(centre),
                      Qt::LeftButton, Qt::LeftButton, event->modifiers());
    QSlider::mousePressEvent(&press);
    setSliderPosition(target);
    event->accept();
}
