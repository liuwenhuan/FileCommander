#include "QuickView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ImageViewer.h"
#include "MpvWidget.h"

namespace {
constexpr qint64 kTextPreviewBytes = 64 * 1024; // cap text previews at 64 KiB
constexpr double kZoomStep = 1.25;
constexpr double kMinScale = 0.05;
constexpr double kMaxScale = 20.0;
} // namespace

QuickView::QuickView(QWidget *parent) : QWidget(parent) {
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);

    m_info = new QLabel(tr("Select a file to preview"), this);
    m_info->setAlignment(Qt::AlignCenter);
    m_info->setWordWrap(true);

    // Coalesce rapid resizes (e.g. dragging the panel divider) into a single
    // smooth rescale so large images don't rescale on every pixel of the drag.
    m_refitTimer = new QTimer(this);
    m_refitTimer->setSingleShot(true);
    m_refitTimer->setInterval(40);
    connect(m_refitTimer, &QTimer::timeout, this, [this]() {
        if (m_imageFitMode && !m_originalPixmap.isNull()) {
            m_imageScale = fitScale();
            applyImageScale();
        }
    });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_info);            // 0
    m_stack->addWidget(buildImagePage());  // 1
    m_stack->addWidget(m_text);            // 2
    m_stack->addWidget(buildVideoPage());  // 3

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
}

QWidget *QuickView::buildImagePage() {
    m_imagePage = new QWidget(this);

    auto *toolbar = new QToolBar(m_imagePage);
    toolbar->addAction(tr("Zoom In"), this, [this]() { zoomImageBy(kZoomStep); });
    toolbar->addAction(tr("Zoom Out"), this, [this]() { zoomImageBy(1.0 / kZoomStep); });
    toolbar->addAction(tr("Fit"), this, [this]() {
        m_imageFitMode = true;
        m_imageScale = fitScale();
        applyImageScale();
    });

    // Push the lock checkbox to the far right of the toolbar.
    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    m_lockZoomCheck = new QCheckBox(tr("Lock Zoom"), toolbar);
    m_lockZoomCheck->setToolTip(tr("Keep the current zoom ratio for the next images"));
    toolbar->addWidget(m_lockZoomCheck);

    m_infoCheck = new QCheckBox(tr("Show info"), toolbar);
    m_infoCheck->setToolTip(tr("Overlay basic image information"));
    toolbar->addWidget(m_infoCheck);
    connect(m_infoCheck, &QCheckBox::toggled, this, [this](bool on) {
        // The overlay only makes sense over an actual image page.
        if (on && m_stack->currentWidget() == m_imagePage && !m_originalPixmap.isNull()) {
            m_infoOverlay->show();
            positionInfoOverlay();
        } else {
            m_infoOverlay->hide();
        }
    });

    m_imageLabel = new QLabel(m_imagePage);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageScroll = new QScrollArea(m_imagePage);
    m_imageScroll->setWidget(m_imageLabel);
    m_imageScroll->setWidgetResizable(false);
    m_imageScroll->setAlignment(Qt::AlignCenter);
    // Wheel to zoom, left-drag to pan: filter both the label (over the image)
    // and the empty viewport around it.
    m_imageLabel->installEventFilter(this);
    m_imageScroll->viewport()->installEventFilter(this);

    // Floating metadata panel, parented to the viewport so it hovers over the
    // image and does not scroll with it.
    m_infoOverlay = new QLabel(m_imageScroll->viewport());
    m_infoOverlay->setStyleSheet(
        "background: rgba(0,0,0,160); color: white; padding: 6px; border-radius: 4px;");
    m_infoOverlay->setTextFormat(Qt::RichText);
    m_infoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_infoOverlay->hide();

    auto *layout = new QVBoxLayout(m_imagePage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_imageScroll, 1);
    return m_imagePage;
}

double QuickView::fitScale() const {
    if (m_originalPixmap.isNull())
        return 1.0;
    const QSize avail = m_imageScroll->viewport()->size();
    const QSize pix = m_originalPixmap.size();
    if (pix.width() <= 0 || pix.height() <= 0)
        return 1.0;
    const double s = qMin(static_cast<double>(avail.width()) / pix.width(),
                          static_cast<double>(avail.height()) / pix.height());
    return qBound(kMinScale, s, kMaxScale);
}

void QuickView::applyImageScale() {
    if (m_originalPixmap.isNull())
        return;
    const QSize target = m_originalPixmap.size() * m_imageScale;
    m_imageLabel->setPixmap(
        m_originalPixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_imageLabel->resize(target);
    // Hint that the image can be dragged when it overflows the viewport.
    const bool pannable = target.width() > m_imageScroll->viewport()->width() ||
                          target.height() > m_imageScroll->viewport()->height();
    m_imageScroll->viewport()->setCursor(pannable ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void QuickView::positionInfoOverlay() {
    if (!m_infoOverlay->isVisible())
        return;
    m_infoOverlay->adjustSize();
    const int vw = m_imageScroll->viewport()->width();
    const int x = qMax(8, vw - m_infoOverlay->width() - 8);
    m_infoOverlay->move(x, 8);
    m_infoOverlay->raise();
}

bool QuickView::isVideo(const QString &path) {
    static const QSet<QString> kVideoSuffixes = {
        "mp4", "mkv", "avi",  "mov", "webm", "flv", "wmv",
        "m4v", "mpg", "mpeg", "ts",  "m2ts", "3gp", "ogv"};
    return kVideoSuffixes.contains(QFileInfo(path).suffix().toLower());
}

QWidget *QuickView::buildVideoPage() {
    m_videoPage = new QWidget(this);

    m_mpv = new MpvWidget(m_videoPage);
    m_mpv->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_mpv->setMinimumHeight(120);
    m_mpv->installEventFilter(this); // reposition the overlay on resize

    // Floating metadata panel, parented to the video widget so it hovers on top.
    m_videoInfoOverlay = new QLabel(m_mpv);
    m_videoInfoOverlay->setStyleSheet(
        "background: rgba(0,0,0,160); color: white; padding: 6px; border-radius: 4px;");
    m_videoInfoOverlay->setTextFormat(Qt::RichText);
    m_videoInfoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_videoInfoOverlay->hide();

    // Control bar: play/pause, speed, progress, volume (muted by default), info.
    m_playButton = new QPushButton(tr("Play"), m_videoPage);
    connect(m_playButton, &QPushButton::clicked, this, [this]() {
        m_mpv->playPause();
        // Reflect the resulting state; playPause is async so query after a beat.
        QTimer::singleShot(50, this, [this]() {
            m_playButton->setText(m_mpv->paused() ? tr("Play") : tr("Pause"));
        });
    });

    m_speedCombo = new QComboBox(m_videoPage);
    m_speedCombo->addItem(tr("1x"), 1.0);
    m_speedCombo->addItem(tr("1.5x"), 1.5);
    m_speedCombo->addItem(tr("2x"), 2.0);
    m_speedCombo->addItem(tr("3x"), 3.0);
    connect(m_speedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { m_mpv->setSpeed(m_speedCombo->itemData(index).toDouble()); });

    m_progressSlider = new QSlider(Qt::Horizontal, m_videoPage);
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setToolTip(tr("Seek"));
    connect(m_progressSlider, &QSlider::sliderPressed, this, [this]() { m_seeking = true; });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this]() {
        m_mpv->seekFraction(m_progressSlider->value() / 1000.0);
        m_seeking = false;
    });

    auto *volumeLabel = new QLabel(tr("Vol"), m_videoPage);
    m_volumeSlider = new QSlider(Qt::Horizontal, m_videoPage);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(70);
    m_volumeSlider->setFixedWidth(90);
    m_volumeSlider->setToolTip(tr("Volume (starts muted)"));
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_mpv->setVolume(value);
        // Any deliberate volume change lifts the initial mute.
        m_mpv->setMute(value == 0);
    });

    m_videoInfoCheck = new QCheckBox(tr("Show info"), m_videoPage);
    m_videoInfoCheck->setToolTip(tr("Overlay basic video information"));
    connect(m_videoInfoCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_stack->currentWidget() == m_videoPage) {
            updateVideoInfoOverlay();
            m_videoInfoOverlay->show();
            positionVideoInfoOverlay();
        } else {
            m_videoInfoOverlay->hide();
        }
    });

    auto *controls = new QHBoxLayout();
    controls->setContentsMargins(4, 2, 4, 2);
    controls->addWidget(m_playButton);
    controls->addWidget(m_speedCombo);
    controls->addWidget(m_progressSlider, 1);
    controls->addWidget(volumeLabel);
    controls->addWidget(m_volumeSlider);
    controls->addWidget(m_videoInfoCheck);

    auto *layout = new QVBoxLayout(m_videoPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_mpv, 1);
    layout->addLayout(controls);

    // Poll the playback position to advance the progress slider and refresh the
    // info overlay while a clip plays.
    m_videoTimer = new QTimer(this);
    m_videoTimer->setInterval(250);
    connect(m_videoTimer, &QTimer::timeout, this, [this]() {
        if (m_seeking)
            return;
        const double dur = m_mpv->durationSeconds();
        if (dur > 0.0) {
            const double frac = m_mpv->positionSeconds() / dur;
            m_progressSlider->setValue(qBound(0, static_cast<int>(frac * 1000.0), 1000));
        }
        if (m_videoInfoOverlay->isVisible())
            updateVideoInfoOverlay();
    });

    return m_videoPage;
}

void QuickView::positionVideoInfoOverlay() {
    if (!m_videoInfoOverlay->isVisible())
        return;
    m_videoInfoOverlay->adjustSize();
    const int vw = m_mpv->width();
    const int x = qMax(8, vw - m_videoInfoOverlay->width() - 8);
    m_videoInfoOverlay->move(x, 8);
    m_videoInfoOverlay->raise();
}

void QuickView::updateVideoInfoOverlay() {
    const double dur = m_mpv->durationSeconds();
    const int totalSec = static_cast<int>(dur + 0.5);
    const QString duration = QString("%1:%2:%3")
                                 .arg(totalSec / 3600, 2, 10, QChar('0'))
                                 .arg((totalSec % 3600) / 60, 2, 10, QChar('0'))
                                 .arg(totalSec % 60, 2, 10, QChar('0'));
    const int w = m_mpv->videoWidth();
    const int h = m_mpv->videoHeight();
    const QString codec = m_mpv->videoCodec();

    const QString text =
        tr("<b>Duration:</b> %1<br><b>Resolution:</b> %2 &times; %3<br><b>Codec:</b> %4")
            .arg(duration)
            .arg(w > 0 ? QString::number(w) : tr("?"))
            .arg(h > 0 ? QString::number(h) : tr("?"))
            .arg(codec.isEmpty() ? tr("unknown") : codec.toHtmlEscaped());
    m_videoInfoOverlay->setText(text);
    positionVideoInfoOverlay();
}

void QuickView::stopVideo() {
    if (!m_mpv)
        return;
    if (m_videoTimer)
        m_videoTimer->stop();
    m_mpv->stop();
    m_videoInfoOverlay->hide();
}

void QuickView::zoomImageBy(double factor) {
    m_imageFitMode = false;
    m_imageScale = qBound(kMinScale, m_imageScale * factor, kMaxScale);
    applyImageScale();
}

bool QuickView::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_mpv && event->type() == QEvent::Resize) {
        positionVideoInfoOverlay(); // keep the panel pinned to the top-right corner
        // fall through to default handling
    }
    const bool onImage =
        watched == m_imageLabel || watched == m_imageScroll->viewport();
    if (watched == m_imageScroll->viewport() && event->type() == QEvent::Resize) {
        positionInfoOverlay(); // keep the panel pinned to the top-right corner
        // fall through to default handling
    }
    if (onImage) {
        switch (event->type()) {
        case QEvent::Wheel: {
            auto *we = static_cast<QWheelEvent *>(event);
            zoomImageBy(we->angleDelta().y() > 0 ? kZoomStep : 1.0 / kZoomStep);
            return true; // consume: wheel zooms rather than scrolls
        }
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_panning = true;
                m_panStart = me->globalPos();
                m_panHScroll = m_imageScroll->horizontalScrollBar()->value();
                m_panVScroll = m_imageScroll->verticalScrollBar()->value();
                m_imageScroll->viewport()->setCursor(Qt::ClosedHandCursor);
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            if (m_panning) {
                auto *me = static_cast<QMouseEvent *>(event);
                const QPoint delta = me->globalPos() - m_panStart;
                m_imageScroll->horizontalScrollBar()->setValue(m_panHScroll - delta.x());
                m_imageScroll->verticalScrollBar()->setValue(m_panVScroll - delta.y());
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (m_panning) {
                m_panning = false;
                applyImageScale(); // restores the open/arrow cursor
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QuickView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // While in fit mode, keep the image sized to the pane, but debounce so a
    // divider drag triggers one rescale at the end rather than one per pixel.
    if (m_imageFitMode && !m_originalPixmap.isNull() && m_stack->currentWidget() == m_imagePage)
        m_refitTimer->start();
}

void QuickView::showFile(const QString &path) {
    QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || info.isDir()) {
        stopVideo();
        m_infoOverlay->hide();
        m_info->setText(tr("Select a file to preview"));
        m_stack->setCurrentWidget(m_info);
        return;
    }

    if (isVideo(path)) {
        m_infoOverlay->hide(); // image overlay belongs to another page
        m_mpv->setMute(true);  // default muted on every new clip
        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(70);
        m_volumeSlider->blockSignals(false);
        m_progressSlider->setValue(0);
        m_speedCombo->setCurrentIndex(0);
        m_mpv->setSpeed(1.0);
        m_playButton->setText(tr("Pause")); // loadfile starts playing
        m_mpv->load(path);
        m_stack->setCurrentWidget(m_videoPage);
        if (m_videoInfoCheck->isChecked()) {
            updateVideoInfoOverlay();
            m_videoInfoOverlay->show();
            positionVideoInfoOverlay();
        } else {
            m_videoInfoOverlay->hide();
        }
        m_videoTimer->start();
        return;
    }

    // Any non-video target: make sure playback is not left running in the back.
    stopVideo();

    if (ImageViewer::isImage(path)) {
        QImageReader reader(path);
        const QSize dim = reader.size();
        const QString format = QString::fromLatin1(reader.format()).toUpper();
        QPixmap pm(path);
        if (!pm.isNull()) {
            m_originalPixmap = pm;

            // Build the metadata panel from what QImageReader/QPixmap expose.
            const QString text =
                tr("<b>%1</b><br>%2 &times; %3<br>%4<br>%5 bpp")
                    .arg(info.fileName().toHtmlEscaped())
                    .arg(dim.isValid() ? dim.width() : pm.width())
                    .arg(dim.isValid() ? dim.height() : pm.height())
                    .arg(format.isEmpty() ? tr("Unknown format") : format)
                    .arg(pm.depth());
            m_infoOverlay->setText(text);

            m_stack->setCurrentWidget(m_imagePage);
            if (m_infoCheck->isChecked()) {
                m_infoOverlay->show();
                positionInfoOverlay();
            } else {
                m_infoOverlay->hide();
            }
            if (m_lockZoomCheck->isChecked()) {
                // Reuse the ratio the user locked in; don't refit.
                m_imageFitMode = false;
            } else {
                m_imageFitMode = true;
                m_imageScale = fitScale();
            }
            applyImageScale();
            return;
        }
    }

    m_originalPixmap = QPixmap();
    m_infoOverlay->hide(); // no image behind it on the text / no-preview pages
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.read(kTextPreviewBytes);
        m_text->setPlainText(QString::fromUtf8(head));
        m_stack->setCurrentWidget(m_text);
        return;
    }

    m_info->setText(tr("No preview available for %1").arg(info.fileName()));
    m_stack->setCurrentWidget(m_info);
}
