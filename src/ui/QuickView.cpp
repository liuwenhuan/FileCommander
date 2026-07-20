#include "QuickView.h"

#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolBar>
#include <QVBoxLayout>

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
    toolbar->addAction(tr("Zoom In"), this, [this]() {
        m_imageFitMode = false;
        m_imageScale = qMin(m_imageScale * kZoomStep, kMaxScale);
        applyImageScale();
    });
    toolbar->addAction(tr("Zoom Out"), this, [this]() {
        m_imageFitMode = false;
        m_imageScale = qMax(m_imageScale / kZoomStep, kMinScale);
        applyImageScale();
    });
    toolbar->addAction(tr("Fit"), this, [this]() {
        m_imageFitMode = true;
        m_imageScale = fitScale();
        applyImageScale();
    });

    m_imageLabel = new QLabel(m_imagePage);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageScroll = new QScrollArea(m_imagePage);
    m_imageScroll->setWidget(m_imageLabel);
    m_imageScroll->setWidgetResizable(false);
    m_imageScroll->setAlignment(Qt::AlignCenter);

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
            m_imageFitMode = true;
            m_stack->setCurrentWidget(m_imagePage);
            m_imageScale = fitScale();
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
