#include "QuickView.h"

#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ImageViewer.h"

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

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_info);            // 0
    m_stack->addWidget(buildImagePage());  // 1
    m_stack->addWidget(m_text);            // 2

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

void QuickView::zoomImageBy(double factor) {
    m_imageFitMode = false;
    m_imageScale = qBound(kMinScale, m_imageScale * factor, kMaxScale);
    applyImageScale();
}

bool QuickView::eventFilter(QObject *watched, QEvent *event) {
    const bool onImage =
        watched == m_imageLabel || watched == m_imageScroll->viewport();
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
    // While in fit mode, keep the image sized to the (now larger/smaller) pane.
    if (m_imageFitMode && !m_originalPixmap.isNull() && m_stack->currentWidget() == m_imagePage) {
        m_imageScale = fitScale();
        applyImageScale();
    }
}

void QuickView::showFile(const QString &path) {
    QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || info.isDir()) {
        m_info->setText(tr("Select a file to preview"));
        m_stack->setCurrentWidget(m_info);
        return;
    }

    if (ImageViewer::isImage(path)) {
        QPixmap pm(path);
        if (!pm.isNull()) {
            m_originalPixmap = pm;
            m_stack->setCurrentWidget(m_imagePage);
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
