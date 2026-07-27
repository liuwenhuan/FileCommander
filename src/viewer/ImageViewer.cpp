#include "ImageViewer.h"

#include "FileInfo.h"

#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeDatabase>
#include <QPixmap>
#include <QScrollArea>
#include <QSet>
#include <QShortcut>
#include <QToolBar>
#include <QVBoxLayout>

namespace {
bool isImageFile(const QString &path) {
    return ImageViewer::isImage(path);
}
} // namespace

bool ImageViewer::isImage(const QString &path) {
    static const QSet<QString> kImageSuffixes = {"png",  "jpg", "jpeg", "gif",
                                                   "bmp",  "svg", "webp", "ico"};
    return kImageSuffixes.contains(
        FileInfo::suffixForName(QFileInfo(path).fileName()).toLower());
}

ImageViewer::ImageViewer(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setBackgroundRole(QPalette::Base);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidget(m_imageLabel);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignCenter);

    auto *toolbar = new QToolBar(this);
    toolbar->addAction(tr("Fit"), this, &ImageViewer::fitToWindow);
    toolbar->addAction(tr("100%"), this, &ImageViewer::actualSize);
    toolbar->addAction(tr("Zoom In"), this, &ImageViewer::zoomIn);
    toolbar->addAction(tr("Zoom Out"), this, &ImageViewer::zoomOut);
    toolbar->addAction(tr("< Prev"), this, &ImageViewer::previousImage);
    toolbar->addAction(tr("Next >"), this, &ImageViewer::nextImage);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(m_scrollArea, 1);

    auto *closeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(closeShortcut, &QShortcut::activated, this, &QWidget::close);
    auto *nextShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
    connect(nextShortcut, &QShortcut::activated, this, &ImageViewer::nextImage);
    auto *prevShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
    connect(prevShortcut, &QShortcut::activated, this, &ImageViewer::previousImage);

    resize(900, 700);
}

bool ImageViewer::loadImage(const QString &path) {
    QPixmap pixmap(path);
    if (pixmap.isNull())
        return false;

    m_pixmap = pixmap;
    m_path = path;
    m_fitToWindow = true;
    setWindowTitle(QFileInfo(path).fileName());
    loadSiblingList();
    applyScale();
    return true;
}

void ImageViewer::loadSiblingList() {
    QDir dir(QFileInfo(m_path).absolutePath());
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    m_siblings.clear();
    for (const QFileInfo &fi : entries) {
        if (isImageFile(fi.absoluteFilePath()))
            m_siblings.append(fi.absoluteFilePath());
    }
    m_siblingIndex = m_siblings.indexOf(QFileInfo(m_path).absoluteFilePath());
}

void ImageViewer::applyScale() {
    if (m_pixmap.isNull())
        return;

    if (m_fitToWindow) {
        const QSize target = m_scrollArea->viewport()->size();
        QPixmap scaled = m_pixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_imageLabel->setPixmap(scaled);
        m_imageLabel->resize(scaled.size());
    } else {
        QPixmap scaled = m_pixmap.scaled(m_pixmap.size() * m_scale, Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
        m_imageLabel->setPixmap(scaled);
        m_imageLabel->resize(scaled.size());
    }
}

void ImageViewer::zoomIn() {
    m_fitToWindow = false;
    m_scale = qMin(m_scale * 1.25, 8.0);
    applyScale();
}

void ImageViewer::zoomOut() {
    m_fitToWindow = false;
    m_scale = qMax(m_scale / 1.25, 0.1);
    applyScale();
}

void ImageViewer::fitToWindow() {
    m_fitToWindow = true;
    applyScale();
}

void ImageViewer::actualSize() {
    m_fitToWindow = false;
    m_scale = 1.0;
    applyScale();
}

void ImageViewer::nextImage() {
    if (m_siblings.isEmpty() || m_siblingIndex < 0)
        return;
    const int next = (m_siblingIndex + 1) % m_siblings.size();
    loadImage(m_siblings.at(next));
}

void ImageViewer::previousImage() {
    if (m_siblings.isEmpty() || m_siblingIndex < 0)
        return;
    const int prev = (m_siblingIndex - 1 + m_siblings.size()) % m_siblings.size();
    loadImage(m_siblings.at(prev));
}

void ImageViewer::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_fitToWindow)
        applyScale();
}

void ImageViewer::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // The viewport doesn't have its final size until the first show, so a
    // fit-to-window scale computed in loadImage() (called before show())
    // would use a stale/default size. Recompute once we're actually shown.
    if (m_fitToWindow)
        applyScale();
}
