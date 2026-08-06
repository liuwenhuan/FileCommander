#include "QuickView.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QClipboard>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QBuffer>
#include <QGraphicsOpacityEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextCodec>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QOpenGLWidget>
#include <QtConcurrent>

#include <exception>
#include <limits>
#include <utility>

#if FILECOMMANDER_HAS_PREVIEW_PDF
#include <poppler-qt5.h>
#endif

#include "ArchiveHandler.h"
#include "ArchiveModel.h"
#include "PackageInfo.h"
#include "ImageViewer.h"
#include "IconCache.h"
#include "FileInfo.h"
#if FILECOMMANDER_MEDIA_BACKEND_MPV
#include "MpvStreamSource.h"
#include "media/MpvMediaEngine.h"
#elif FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF
#include "media/WindowsMediaEngine.h"
#else
#include "media/NullMediaEngine.h"
#endif
#include "theme/Phosphor.h"
#include "theme/PhosphorEffect.h"
#include "OfficeConverter.h"
#include "MotionPolicy.h"
#include "SeekSlider.h"
#include "SlideSceneBuilder.h"
#include "TextEncodingDetector.h"
#include "config/Settings.h"
#include "media/Id3Reader.h"
#include "media/MediaEngine.h"

namespace {
constexpr qint64 kTextWindowBytes = 5 * 1024 * 1024; // text preview cap: 5 MiB
constexpr int kHexWindowBytes = 256 * 1024;           // hex expands to ~4x text
// All supported encodings consume at most four bytes per code point. Reading
// this small look-ahead lets safePrefix cut at a complete character boundary.
constexpr qint64 kTextReadLookAheadBytes = 4;
constexpr qint64 kMarkdownMaxBytes = 2 * 1024 * 1024; // cap markdown at 2 MiB

// office_oxide (and Markdown) tables carry no cell borders; QTextDocument draws
// none by default. This default stylesheet gives every table cell a thin border
// so Word/Excel/Markdown tables read as grids. Shared by the on-screen browser
// and the off-thread render document so both lay out identically.
const QString kMarkdownDefaultCss =
    QStringLiteral("table { border-collapse: collapse; } "
                   "td, th { border: 1px solid #808080; padding: 2px 6px; }");

// Selectable text encodings for the text-preview page. The Auto entry asks
// TextEncodingDetector to choose for the current file; all other entries are
// deliberate one-file overrides. The table itself lives in core/text beside the
// detector, so this toolbar and the F4 editor's offer the same list.
using TextEncoding = TextEncodingDetector::Selectable;
constexpr auto &kTextEncodings = TextEncodingDetector::selectableEncodings;
constexpr double kZoomStep = 1.25;

// Floors for the video transport's two sliders. The seek bar is the control a
// narrow preview pane must not take away -- a 20px seek bar cannot be seeked
// with -- so it keeps the larger floor and volume gives way first.
constexpr int kMinSeekWidth = 160;
constexpr int kMinVolumeWidth = 44;
constexpr double kMinScale = 0.05;
constexpr double kMaxScale = 20.0;
// PDF rendering: Poppler's renderToImage takes dpi; 72 dpi renders a page at
// its native point size (1.0 zoom). We scale that base by the zoom factor.
constexpr double kPdfBaseDpi = 72.0;
constexpr double kPdfMinZoom = 0.25;
constexpr double kPdfMaxZoom = 6.0;
// Side gutter reserved so a fitted page never triggers a horizontal scrollbar.
constexpr int kPdfSideMargin = 24;
// Vertical gap (device px) between stacked PDF page bitmaps in the scene.
constexpr int kPdfPageGap = 12;
// Slides rendered in the fast first stage of a pptx preview; the rest stream in
// afterwards (see QuickView::renderOffice).
constexpr int kFirstStageSlides = 3;

} // namespace

QuickView::QuickView(Settings &settings, Context context, QWidget *parent,
                     std::unique_ptr<MediaEngine> mediaEngine)
    : QWidget(parent), m_settings(settings), m_context(context) {
    // QuickView is the single painted surface behind its stacked pages. The CRT
    // theme tiles this widget once and keeps the default page/stack transparent,
    // so scanlines stay continuous instead of restarting in each child.
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName(QStringLiteral("QuickViewSurface"));
    // Both contexts read up to 5 MiB and show the same toolbar, so the embedded
    // Ctrl+Q pane and the F3 window preview text files identically.
    Q_UNUSED(context);
    m_textCap = kTextWindowBytes;

    m_info = new QLabel(tr("Select a file to preview"), this);
    m_info->setObjectName(QStringLiteral("previewInfoLabel"));
    m_info->setAlignment(Qt::AlignCenter);
    m_info->setWordWrap(true);

    m_imageLoader = new ImagePreviewLoader(this);

    // Coalesce rapid resizes (e.g. dragging the panel divider) into a single
    // smooth rescale so large images don't rescale on every pixel of the drag.
    m_refitTimer = new QTimer(this);
    m_refitTimer->setSingleShot(true);
    m_refitTimer->setInterval(40);
    connect(m_refitTimer, &QTimer::timeout, this, [this]() {
        if (m_imageFitMode && !m_originalImage.isNull()) {
            m_imageScale = fitScale();
            applyImageScale();
        }
    });

    m_imageWheelRenderTimer = new QTimer(this);
    m_imageWheelRenderTimer->setObjectName(QStringLiteral("imageWheelRenderTimer"));
    m_imageWheelRenderTimer->setSingleShot(true);
    m_imageWheelRenderTimer->setInterval(50);
    connect(m_imageWheelRenderTimer, &QTimer::timeout, this,
            &QuickView::requestImageRender);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("QuickViewStack"));
    m_stack->addWidget(m_info);

    if (mediaEngine) {
        m_pendingMediaEngine = std::move(mediaEngine);
    } else {
#if !FILECOMMANDER_HAS_PREVIEW_MEDIA
        // The null backend owns no native media resources. Keep disabled-backend
        // selection observable while deferring initialization and media pages.
        m_mediaEngine = new NullMediaEngine(this);
#endif
    }

    m_stack->addWidget(buildImagePage());
    m_stack->addWidget(buildTextPage());
    m_stack->addWidget(buildMarkdownPage());
    m_stack->addWidget(buildEncryptedPage());
    m_stack->addWidget(buildDownloadPage());

    connect(m_imageLoader, &ImagePreviewLoader::loaded, this,
            [this](quint64 generation, QImage image, const ImageMetadata &metadata,
                   const QString &error) {
                if (generation != m_pendingImageLoadGeneration)
                    return;
                m_pendingImageLoadGeneration = 0;
                if (!error.isEmpty() || image.isNull()) {
                    m_originalImage = {};
                    m_imageLabel->clear();
                    clearImageTransitionSnapshot();
                    m_infoOverlay->hide();
                    m_info->setText(tr("No preview available for %1")
                                        .arg(QFileInfo(m_pendingImagePath).fileName()));
                    m_imageRevealPending = false;
                    revealStaticPage(m_info);
                    return;
                }

                m_originalImage = std::move(image);
                m_imageMetadata = metadata;
                m_imageTransform.reset();
                m_imagePath = m_pendingImagePath;
                loadImageSiblings();
                updateImageInfoOverlay();
                if (m_infoCheck->isChecked()) {
                    m_infoOverlay->show();
                    positionInfoOverlay();
                } else {
                    m_infoOverlay->hide();
                }
                if (m_lockZoomCheck->isChecked()) {
                    m_imageFitMode = false;
                } else {
                    m_imageFitMode = true;
                    m_imageScale = fitScale();
                }
                requestImageRender();
            });
    connect(m_imageLoader, &ImagePreviewLoader::rendered, this,
            [this](quint64 generation, QImage image) {
                if (generation != m_pendingImageRenderGeneration || image.isNull() ||
                    (!m_imageRevealPending && m_stack->currentWidget() != m_imagePage))
                    return;
                m_pendingImageRenderGeneration = 0;

                const QColor tint = fc::previewTint();
                if (tint.isValid()) {
                    if (image.format() != QImage::Format_ARGB32)
                        image = image.convertToFormat(QImage::Format_ARGB32);
                    fc::tintImage(image, tint);
                    fc::applyScanlines(image);
                }
                m_imageLabel->setPixmap(QPixmap::fromImage(image));
                m_imageLabel->resize(image.size());
                clearImageTransitionSnapshot();
                updateImageCursor(image.size());
                if (m_imageRevealPending) {
                    m_imageRevealPending = false;
                    revealStaticPage(m_imagePage);
                }
            });
    connect(m_imageLoader, &ImagePreviewLoader::rotationPersisted, this,
            [this](const QString &path, bool saved) {
                const int pending = m_pendingImageRotations.value(path);
                if (pending <= 1)
                    m_pendingImageRotations.remove(path);
                else
                    m_pendingImageRotations.insert(path, pending - 1);
                if (!saved && path == m_imagePath && !m_originalImage.isNull()) {
                    m_infoOverlay->setText(
                        tr("Rotated on screen only - could not save to disk."));
                    m_infoOverlay->show();
                    positionInfoOverlay();
                }
            });
    connect(m_stack, &QStackedWidget::currentChanged, this, [this](int index) {
        if (m_stack->widget(index) != m_imagePage)
            invalidateImageRequests();
    });
    MotionPolicy::observeReduced(this, [this](bool reduced) {
        if (reduced)
            finishStaticReveal();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
}

// Defined here (not defaulted in the header) so the Poppler document holder is
// destroyed where the complete Poppler type is visible.
QuickView::~QuickView() {
    stopPlayback();
    finishStaticReveal();
    invalidateImageRequests();
}

bool QuickView::isStaticPageEligible(QWidget *page) const {
    if (!page)
        return false;

    const bool approved =
        page == m_imagePage || page == m_textPage || page == m_markdown ||
        page == m_pdfPage || page == m_officeTabs || page == m_archivePage ||
        page == m_slidesPage || page == m_info;
    if (!approved)
        return false;

    const auto isForbiddenSurface = [](QWidget *widget) {
        return qobject_cast<QOpenGLWidget *>(widget) ||
               widget->inherits("MpvVideoSurface") || widget->inherits("QVideoWidget");
    };
    if (isForbiddenSurface(page))
        return false;
    for (QWidget *child : page->findChildren<QWidget *>()) {
        if (isForbiddenSurface(child))
            return false;
    }
    return true;
}

QWidget *QuickView::staticContentTarget(QWidget *page) const {
    if (page == m_imagePage)
        return m_imageScroll ? m_imageScroll->viewport() : nullptr;
    if (page == m_textPage)
        return m_text ? m_text->viewport() : nullptr;
    if (page == m_markdown)
        return m_markdown ? m_markdown->viewport() : nullptr;
    if (page == m_pdfPage)
        return m_pdfView ? m_pdfView->viewport() : nullptr;
    if (page == m_archivePage)
        return m_archiveView ? m_archiveView->viewport() : nullptr;
    if (page == m_slidesPage)
        return m_slidesView ? m_slidesView->viewport() : nullptr;
    if (page == m_officeTabs) {
        auto *table = qobject_cast<QAbstractScrollArea *>(
            m_officeTabs ? m_officeTabs->currentWidget() : nullptr);
        return table ? table->viewport() : nullptr;
    }
    if (page == m_info)
        return m_info;
    return nullptr;
}

void QuickView::finishStaticReveal() {
    QPropertyAnimation *animation = m_staticRevealAnimation;
    m_staticRevealAnimation = nullptr;
    if (animation) {
        animation->stop();
        animation->setTargetObject(nullptr);
        animation->deleteLater();
    }

    QWidget *target = m_staticRevealTarget.data();
    m_staticRevealTarget.clear();
    if (!target)
        return;
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(target->graphicsEffect());
    if (!effect)
        return;
    effect->setOpacity(1.0);
    target->setGraphicsEffect(nullptr);
}

void QuickView::revealStaticPage(QWidget *page) {
    finishStaticReveal();
    if (!page)
        return;
    if (m_stack->indexOf(page) >= 0) {
        m_stack->setCurrentWidget(page);
        releaseHiddenDocumentPages(page);
    }
    if (!isStaticPageEligible(page))
        return;
    QWidget *target = staticContentTarget(page);
    if (!target)
        return;

    const int duration = MotionPolicy::duration(MotionDuration::Fast);
    if (duration <= 0)
        return;

    auto *effect = new QGraphicsOpacityEffect;
    effect->setOpacity(0.85);
    target->setGraphicsEffect(effect);

    auto *animation = new QPropertyAnimation(effect, "opacity", this);
    animation->setDuration(duration);
    animation->setStartValue(0.85);
    animation->setEndValue(1.0);
    animation->setEasingCurve(MotionPolicy::easing());
    m_staticRevealAnimation = animation;
    m_staticRevealTarget = target;
    connect(animation, &QPropertyAnimation::finished, this, [this, animation]() {
        if (animation == m_staticRevealAnimation)
            finishStaticReveal();
    });
    animation->start();
}

void QuickView::releaseHiddenDocumentPages(QWidget *page) {
    if (page != m_pdfPage)
        closePdf();
    if (page != m_slidesPage)
        closeSlides();
}

void QuickView::cancelPendingPreviewWork() {
    finishStaticReveal();
    invalidateImageRequests();
    ++m_markdownGen;
    ++m_archiveGen;
    if (m_archiveCancel)
        m_archiveCancel->store(true);
    ++m_officeGen;
    ++m_pdfGen;
    if (m_officeConvertTimer)
        m_officeConvertTimer->stop();
    m_pendingOfficePath.clear();
    m_pendingOfficePassword.clear();
}

void QuickView::warmMediaEngine() {
    if (m_mediaEngineReady)
        return;
    if (m_mediaEngineFailed) {
        showMediaEngineFailure();
        return;
    }

    QElapsedTimer elapsed;
    elapsed.start();

    if (!m_mediaEngine) {
        if (m_pendingMediaEngine) {
            m_mediaEngine = m_pendingMediaEngine.release();
        } else {
#if FILECOMMANDER_MEDIA_BACKEND_MPV
            m_mediaEngine = new MpvMediaEngine;
#elif FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF
            m_mediaEngine = new WindowsMediaEngine;
#else
            m_mediaEngine = new NullMediaEngine;
#endif
        }
        m_mediaEngine->setParent(this);
    }

    connect(m_mediaEngine, &MediaEngine::errorOccurred, this,
            [this](const QString &message) {
                if (message.isEmpty())
                    return;
#if FILECOMMANDER_MEDIA_BACKEND_MPV
                const QString source = m_mediaEngine->currentSource().path;
                if (MpvStreamSource::isStreamUrl(source)) {
                    m_videoPath.clear();
                    emit streamFailed(source);
                    return;
                }
#endif
                if (m_videoTimer)
                    m_videoTimer->stop();
                if (m_audioTimer)
                    m_audioTimer->stop();
                const QString help = m_mediaEngine->lastErrorHelpUrl();
                if (help.isEmpty()) {
                    m_info->setTextFormat(Qt::PlainText);
                    m_info->setText(message);
                } else {
                    // The message is escaped, the link is ours: a file name can
                    // contain angle brackets and would otherwise be eaten as
                    // markup.
                    m_info->setTextFormat(Qt::RichText);
                    m_info->setOpenExternalLinks(true);
                    m_info->setText(message.toHtmlEscaped() +
                                    QStringLiteral("<br><br><a href=\"%1\">%2</a>")
                                        .arg(help.toHtmlEscaped(),
                                             tr("Download a decoder").toHtmlEscaped()));
                }
                revealStaticPage(m_info);
            });

    // The backend learns that the length it reported is unusable only once
    // playback runs past it, so this arrives mid-clip rather than at load.
    connect(m_mediaEngine, &MediaEngine::durationChanged, this, [this](double) {
        if (m_mediaEngine->durationIsUnknown())
            applyUnknownDuration();
    });

    // The backend accepted a seek and then could not finish it, so it reloaded
    // the clip; playback is alive again, from the start. Say so instead of
    // leaving the user to work out why the film jumped back to the beginning.
    // The usual cause is that there is nothing at that point to play -- an
    // unfinished download keeps its full size and reads back as zeros -- so
    // the wording points at the file rather than at the decoder.
    connect(m_mediaEngine, &MediaEngine::seekUnsupported, this, [this]() {
        m_seeking = false;
        if (m_progressSlider)
            m_progressSlider->setValue(0);
        showVideoNotice(tr("Nothing could be read at that point in the file — it may be "
                           "incomplete or damaged. Playback restarted from the beginning."));
    });

    // The file has nothing where playback needs it. Playback is stopped by the
    // backend at that point, so this replaces neither the page nor the frame
    // already on screen -- it explains why that frame stopped moving.
    connect(m_mediaEngine, &MediaEngine::mediaIncomplete, this, [this]() {
        m_seeking = false;
        if (m_videoTimer)
            m_videoTimer->stop();
        if (m_playButton)
            m_playButton->setText(tr("Play"));
        showVideoNotice(tr("This file is incomplete — the rest of it was never "
                           "written — so it cannot play through."));
    });

    try {
        m_mediaEngine->initialize();
    } catch (const std::exception &error) {
        m_mediaEngineFailed = true;
        m_mediaEngineFailureMessage = QString::fromUtf8(error.what());
        showMediaEngineFailure();
        emit mediaEngineWarmFailed(m_mediaEngineFailureMessage);
        return;
    } catch (...) {
        m_mediaEngineFailed = true;
        m_mediaEngineFailureMessage = tr("Unknown media backend initialization error.");
        showMediaEngineFailure();
        emit mediaEngineWarmFailed(m_mediaEngineFailureMessage);
        return;
    }
    m_mediaEngineReady = true;
    emit mediaEngineWarmed(elapsed.elapsed());
}

void QuickView::showMediaEngineFailure() {
    if (m_videoTimer)
        m_videoTimer->stop();
    if (m_audioTimer)
        m_audioTimer->stop();
    m_info->setText(
        tr("Media preview could not start.\n\n%1\n\n"
           "Restart File Commander to retry. If the problem continues, "
           "verify that the mpv media backend is installed correctly.")
            .arg(m_mediaEngineFailureMessage));
    revealStaticPage(m_info);
}

QWidget *QuickView::ensurePdfPage() {
    if (!m_pdfPage)
        m_stack->addWidget(buildPdfPage());
    return m_pdfPage;
}

QWidget *QuickView::ensureOfficePage() {
    if (!m_officeTabs)
        m_stack->addWidget(buildOfficeTablePage());
    return m_officeTabs;
}

QWidget *QuickView::ensureArchivePage() {
    if (!m_archivePage)
        m_stack->addWidget(buildArchivePage());
    return m_archivePage;
}

QWidget *QuickView::ensureSlidesPage() {
    if (!m_slidesPage)
        m_stack->addWidget(buildSlidesPage());
    return m_slidesPage;
}

void QuickView::setContentFontSize(int pt) {
    if (pt <= 0)
        return;
    m_contentFontSize = pt;
    // The widget font persists across setPlainText()/setHtml()/setMarkdown(), so
    // setting it here is enough for current and future content.
    if (m_text) {
        QFont f = m_text->font(); // keep the monospace family, change only size
        f.setPointSize(pt);
        m_text->setFont(f);
    }
    if (m_markdown) {
        QFont f = m_markdown->font();
        f.setPointSize(pt);
        m_markdown->setFont(f);
    }
    if (m_officeTabs) {
        // Spreadsheet (xls/xlsx) preview grids: scale every worksheet tab's cell
        // text with the app font. Cells inherit the widget font, so setting it is
        // enough; nudge the row height so larger text isn't clipped. The tab widget
        // itself carries the font too, so new tabs inherit the current size.
        QFont f = m_officeTabs->font();
        f.setPointSize(pt);
        m_officeTabs->setFont(f);
        for (int i = 0; i < m_officeTabs->count(); ++i) {
            auto *table = qobject_cast<QTableWidget *>(m_officeTabs->widget(i));
            if (!table)
                continue;
            table->setFont(f);
            table->verticalHeader()->setDefaultSectionSize(QFontMetrics(f).height() + 6);
            if (table->rowCount() > 0)
                table->resizeColumnsToContents(); // re-fit widths to the new size
        }
    }
}

void QuickView::applyChromeFont(const QFont &font) {
    // Scoped to the toolbars on purpose. QuickView holds two independently
    // configured font domains -- this chrome, and the previewed content that
    // setContentFontSize() drives from the file-list setting -- so a font pass
    // over the whole subtree would silently resize the content too.
    //
    // Each toolbar is walked explicitly rather than left to inherit: the pages
    // are built lazily, and by the time the interface font changes the parent
    // usually already reports the new font (the application font is set first),
    // so setFont() on the parent is a no-op that delivers no FontChange to the
    // widgets docked inside.
    for (QToolBar *toolbar : findChildren<QToolBar *>()) {
        if (toolbar->font() != font)
            toolbar->setFont(font);
        for (QWidget *child : toolbar->findChildren<QWidget *>()) {
            if (child->font() != font)
                child->setFont(font);
        }
    }
}

void QuickView::setContentFontFamily(const QString &family) {
    const QString effectiveFamily = family.isEmpty() ? QApplication::font().family() : family;
    m_contentFontFamily = effectiveFamily;
    auto applyFamily = [&effectiveFamily](QWidget *widget) {
        if (!widget)
            return;
        QFont font = widget->font();
        font.setFamily(effectiveFamily);
        widget->setFont(font);
    };
    applyFamily(m_markdown);
    applyFamily(m_officeTabs);
    for (int i = 0; m_officeTabs && i < m_officeTabs->count(); ++i)
        applyFamily(m_officeTabs->widget(i));
}

void QuickView::focusPreview() {
    QWidget *page = m_stack->currentWidget();
    if (!page)
        return;
    // Prefer each page's primary interactive widget so the keyboard lands on
    // something useful (scroll text, navigate the grid, type the password).
    QWidget *target = nullptr;
    if (page == m_textPage)
        target = m_text;
    else if (page == m_markdown)
        target = m_markdown;
    else if (page == m_imagePage)
        target = m_imageScroll;
    else if (page == m_pdfPage)
        target = m_pdfView;
    else if (page == m_officeTabs)
        target = currentOfficeTable();
    else if (page == m_encryptedPage)
        target = m_passwordEdit;
    else if (page == m_archivePage)
        target = m_archiveView;
    else if (page == m_videoPage)
        target = m_playButton ? static_cast<QWidget *>(m_playButton) : nullptr;
    // Fall back to the first tab-focusable visible descendant, else the page.
    if (!target || !target->isVisible() || target->focusPolicy() == Qt::NoFocus) {
        target = nullptr;
        for (QWidget *w : page->findChildren<QWidget *>())
            if (w->isVisible() && (w->focusPolicy() & Qt::TabFocus)) {
                target = w;
                break;
            }
        if (!target)
            target = page;
    }
    target->setFocus(Qt::TabFocusReason);
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
    toolbar->addAction(tr("Rotate Left"), this, [this]() { rotateCurrentImage(-90); });
    toolbar->addAction(tr("Rotate Right"), this, [this]() { rotateCurrentImage(90); });

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
        if (on && m_stack->currentWidget() == m_imagePage && !m_originalImage.isNull()) {
            m_infoOverlay->show();
            positionInfoOverlay();
        } else {
            m_infoOverlay->hide();
        }
    });

    m_imageLabel = new QLabel(m_imagePage);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageScroll = new QScrollArea(m_imagePage);
    m_imageScroll->setObjectName(QStringLiteral("imagePreviewScroll"));
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
    // Styled by the theme sheets, not here. An inline stylesheet would win over
    // the application one, and palette(...) is no use either: the themes are
    // applied with qApp->setStyleSheet() and do not touch the palette, so
    // palette(window-text) resolves to the same default under all three.
    m_infoOverlay->setObjectName(QStringLiteral("quickViewInfoOverlay"));
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
    if (m_originalImage.isNull())
        return 1.0;
    const QSize avail = m_imageScroll->viewport()->size();
    const QSize pix = transformedImageSize();
    if (pix.width() <= 0 || pix.height() <= 0)
        return 1.0;
    const double s = qMin(static_cast<double>(avail.width()) / pix.width(),
                          static_cast<double>(avail.height()) / pix.height());
    return qBound(kMinScale, s, kMaxScale);
}

void QuickView::applyImageScale() {
    requestImageRender();
}

QSize QuickView::transformedImageSize() const {
    if (m_originalImage.isNull())
        return {};
    const QRectF bounds =
        m_imageTransform.mapRect(QRectF(QPointF(0, 0), QSizeF(m_originalImage.size())));
    return QSize(qRound(bounds.width()), qRound(bounds.height()));
}

void QuickView::requestImageRender() {
    if (m_originalImage.isNull() ||
        (!m_imageRevealPending && m_stack->currentWidget() != m_imagePage))
        return;

    const QSize transformed = transformedImageSize();
    const QSize target(qMax(1, qRound(transformed.width() * m_imageScale)),
                       qMax(1, qRound(transformed.height() * m_imageScale)));
    m_pendingImageRenderGeneration =
        m_imageLoader->render(m_originalImage, target, m_imageTransform);
    m_imageGeneration = qMax(m_imageGeneration, m_pendingImageRenderGeneration);
}

void QuickView::preserveImageTransitionSnapshot() {
    clearImageTransitionSnapshot();
    if (!m_imageScroll || !m_imageScroll->viewport())
        return;

    QWidget *viewport = m_imageScroll->viewport();
    m_imageTransitionSnapshot = new QLabel(viewport);
    m_imageTransitionSnapshot->setObjectName(QStringLiteral("imageTransitionSnapshot"));
    m_imageTransitionSnapshot->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_imageTransitionSnapshot->setGeometry(viewport->rect());
    m_imageTransitionSnapshot->setPixmap(viewport->grab());
    m_imageTransitionSnapshot->show();
    m_imageTransitionSnapshot->raise();
}

void QuickView::clearImageTransitionSnapshot() {
    delete m_imageTransitionSnapshot;
    m_imageTransitionSnapshot = nullptr;
}

void QuickView::invalidateImageRequests() {
    if (m_refitTimer)
        m_refitTimer->stop();
    if (m_imageWheelRenderTimer)
        m_imageWheelRenderTimer->stop();
    if (m_imageLoader)
        m_imageLoader->cancelBefore(++m_imageGeneration);
    m_pendingImageLoadGeneration = 0;
    m_pendingImageRenderGeneration = 0;
    m_pendingImagePath.clear();
    m_imageRevealPending = false;
    clearImageTransitionSnapshot();
}

void QuickView::updateImageInfoOverlay() {
    if (m_originalImage.isNull() || m_imagePath.isEmpty())
        return;
    const QSize dimensions = transformedImageSize();
    const QString format =
        m_imageMetadata.format.isEmpty() ? tr("Unknown format") : m_imageMetadata.format;
    m_infoOverlay->setText(tr("<b>%1</b><br>%2 &times; %3<br>%4<br>%5 bpp")
                               .arg(QFileInfo(m_imagePath).fileName().toHtmlEscaped())
                               .arg(dimensions.width())
                               .arg(dimensions.height())
                               .arg(format)
                               .arg(m_imageMetadata.depth));
    positionInfoOverlay();
}

void QuickView::updateImageCursor(const QSize &displaySize) {
    const bool pannable = displaySize.width() > m_imageScroll->viewport()->width() ||
                          displaySize.height() > m_imageScroll->viewport()->height();
    m_imageScroll->viewport()->setCursor(pannable ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void QuickView::rotateCurrentImage(int degrees) {
    if (m_originalImage.isNull() || m_imagePath.isEmpty())
        return;

    m_imageTransform.rotate(degrees);
    if (m_imageFitMode)
        m_imageScale = fitScale();
    requestImageRender();
    updateImageInfoOverlay();

    const QString path = m_imagePath;
    m_pendingImageRotations.insert(path, m_pendingImageRotations.value(path) + 1);
    m_imageLoader->persistRotation(path, degrees);
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

QWidget *QuickView::buildTextPage() {
    m_textPage = new QWidget(this);
    m_textToolbar = new QToolBar(m_textPage);

    m_textEncoding = new QComboBox(m_textToolbar);
    // A plain QListView popup honours our QSS `::item` colours; the platform's
    // native combo popup ignores them and paints from the palette Text role,
    // which turns non-selected rows invisible in the light theme.
    m_textEncoding->setView(new QListView(m_textEncoding));
    for (const TextEncoding &e : kTextEncodings)
        m_textEncoding->addItem(QString::fromLatin1(e.label));
    m_textToolbar->addWidget(m_textEncoding);
    m_textEncodingStatus = new QLabel(m_textToolbar);
    m_textEncodingStatus->setObjectName(QStringLiteral("textEncodingStatus"));
    m_textToolbar->addWidget(m_textEncodingStatus);
    connect(m_textEncoding, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { renderText(); });

    QAction *wrap = m_textToolbar->addAction(tr("Wrap"));
    wrap->setCheckable(true);
    connect(wrap, &QAction::toggled, this, [this](bool on) {
        m_text->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    });

    QAction *hex = m_textToolbar->addAction(tr("Hex"));
    hex->setCheckable(true);
    connect(hex, &QAction::toggled, this, [this](bool on) {
        m_textHex = on;
        m_textEncoding->setEnabled(!on); // encoding is meaningless in hex mode
        renderText();
    });

    m_textToolbar->addSeparator();
    m_textFind = new QLineEdit(m_textToolbar);
    m_textFind->setPlaceholderText(tr("Find… (Enter / F3)"));
    m_textFind->setClearButtonEnabled(true);
    m_textToolbar->addWidget(m_textFind);
    connect(m_textFind, &QLineEdit::returnPressed, this, &QuickView::findNext);

    m_text = new QPlainTextEdit(m_textPage);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFont(QStringLiteral("monospace")));

    // Shown in both contexts so the embedded Ctrl+Q pane and the F3 window are
    // consistent for text files (same encoding/hex/wrap/find controls). The
    // toolbar is added to the layout below and visible by default.

    auto *layout = new QVBoxLayout(m_textPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_textToolbar);
    layout->addWidget(m_text, 1);
    return m_textPage;
}

QString QuickView::toHexDump(const QByteArray &data) {
    QString out;
    const qint64 reserveCharacters = static_cast<qint64>(data.size()) * 4;
    out.reserve(static_cast<int>(qMin(reserveCharacters,
                                      static_cast<qint64>(std::numeric_limits<int>::max()))));
    for (int offset = 0; offset < data.size(); offset += 16) {
        out += QStringLiteral("%1  ").arg(offset, 8, 16, QLatin1Char('0'));
        QString ascii;
        for (int i = 0; i < 16; ++i) {
            if (offset + i < data.size()) {
                const uchar b = static_cast<uchar>(data.at(offset + i));
                out += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
                ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QLatin1Char('.');
            } else {
                out += QStringLiteral("   ");
            }
        }
        out += QLatin1Char(' ') + ascii + QLatin1Char('\n');
    }
    return out;
}

void QuickView::renderText() {
    const int encodingIndex = m_textEncoding->currentIndex();
    const bool autoEncoding = encodingIndex == 0;
    TextEncodingDetector::Result detected;
    bool displayHex = m_textHex;

    if (autoEncoding) {
        detected = m_textAutoResult;
        if (!m_textAutoResultValid)
            detected = {QStringLiteral("Unknown"), QByteArrayLiteral("UTF-8"), 0, false, true};
        if (detected.binary) {
            m_textEncodingStatus->setText(tr("Auto: Binary (Hex)"));
            displayHex = true;
        } else {
            QString status = tr("Auto: %1").arg(detected.label);
            if (detected.ambiguous)
                status += tr(" (ambiguous)");
            m_textEncodingStatus->setText(status);
        }
    } else {
        const QString label = QString::fromLatin1(kTextEncodings[encodingIndex].label);
        m_textEncodingStatus->setText(displayHex ? tr("Manual: %1 (Hex)").arg(label)
                                              : tr("Manual: %1").arg(label));
    }

    QString content;
    bool renderTruncated = m_textTruncated;
    if (displayHex) {
        const int hexBytes = qMin(m_textRaw.size(), kHexWindowBytes);
        content = toHexDump(m_textRaw.left(hexBytes));
        renderTruncated = renderTruncated || hexBytes < m_textRaw.size();
    } else if (autoEncoding) {
        content = TextEncodingDetector::decode(m_textRaw, detected);
    } else {
        QTextCodec *codec = TextEncodingDetector::codecForSelectableIndex(encodingIndex);
        content = codec ? codec->toUnicode(m_textRaw) : QString::fromUtf8(m_textRaw);
    }
    if (renderTruncated)
        content += tr("\n\n[... truncated ...]");
    m_text->setPlainText(content);
}

void QuickView::findNext() {
    if (m_stack->currentWidget() != m_textPage || !m_textFind)
        return;
    const QString needle = m_textFind->text();
    if (needle.isEmpty())
        return;
    if (!m_text->find(needle)) {
        m_text->moveCursor(QTextCursor::Start); // wrap around
        m_text->find(needle);
    }
}

void QuickView::loadImageSiblings() {
    const QDir dir(QFileInfo(m_imagePath).absolutePath());
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    m_imageSiblings.clear();
    for (const QFileInfo &fi : entries)
        if (ImageViewer::isImage(fi.absoluteFilePath()))
            m_imageSiblings.append(fi.absoluteFilePath());
    m_imageSiblingIndex = m_imageSiblings.indexOf(QFileInfo(m_imagePath).absoluteFilePath());
}

void QuickView::showNextSibling() {
    // Audio page: step to the next track in the directory.
    if (m_stack->currentWidget() == m_audioPage) {
        if (m_audioSiblings.isEmpty() || m_audioSiblingIndex < 0)
            return;
        showFile(m_audioSiblings.at((m_audioSiblingIndex + 1) % m_audioSiblings.size()));
        return;
    }
    if (m_stack->currentWidget() != m_imagePage || m_imageSiblings.isEmpty() ||
        m_imageSiblingIndex < 0)
        return;
    showFile(m_imageSiblings.at((m_imageSiblingIndex + 1) % m_imageSiblings.size()));
}

void QuickView::showPrevSibling() {
    // Audio page: step to the previous track in the directory.
    if (m_stack->currentWidget() == m_audioPage) {
        if (m_audioSiblings.isEmpty() || m_audioSiblingIndex < 0)
            return;
        const int prev =
            (m_audioSiblingIndex - 1 + m_audioSiblings.size()) % m_audioSiblings.size();
        showFile(m_audioSiblings.at(prev));
        return;
    }
    if (m_stack->currentWidget() != m_imagePage || m_imageSiblings.isEmpty() ||
        m_imageSiblingIndex < 0)
        return;
    const int prev = (m_imageSiblingIndex - 1 + m_imageSiblings.size()) % m_imageSiblings.size();
    showFile(m_imageSiblings.at(prev));
}

bool QuickView::isVideo(const QString &path) {
    static const QSet<QString> kVideoSuffixes = {
        "mp4", "mkv", "avi",  "mov", "webm", "flv", "wmv",
        "m4v", "mpg", "mpeg", "ts",  "m2ts", "3gp", "ogv"};
    return kVideoSuffixes.contains(FileInfo::suffixForName(QFileInfo(path).fileName()).toLower());
}

bool QuickView::isMarkdown(const QString &path) {
    static const QSet<QString> kMarkdownSuffixes = {"md", "markdown", "mkd", "mdown"};
    return kMarkdownSuffixes.contains(FileInfo::suffixForName(QFileInfo(path).fileName()).toLower());
}

bool QuickView::isPdf(const QString &path) {
    return FileInfo::suffixForName(QFileInfo(path).fileName()).compare(
               QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

bool QuickView::isAudio(const QString &path) {
    static const QSet<QString> kAudioSuffixes = {"mp3", "wav",  "flac", "ogg",
                                                 "aac", "m4a", "wma",  "opus"};
    return kAudioSuffixes.contains(FileInfo::suffixForName(QFileInfo(path).fileName()).toLower());
}

void QuickView::rotateVideoBy(int degrees) {
    m_videoRotation = ((m_videoRotation + degrees) % 360 + 360) % 360;
    if (m_mediaEngine)
        m_mediaEngine->setVideoRotation(m_videoRotation);
}

bool QuickView::waitForTextIdleForTest(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (m_textLoadPending && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return !m_textLoadPending;
}

QWidget *QuickView::buildVideoPageForTest() {
    // The page wires itself to the media engine, so the engine has to exist
    // first -- warmMediaEngine() is what promotes an injected one (or builds
    // the real backend). Null when no engine could be made, rather than a
    // half-built page.
    warmMediaEngine();
    return m_mediaEngine ? ensureVideoPage() : nullptr;
}

QWidget *QuickView::ensureVideoPage() {
    if (!m_videoPage)
        m_stack->addWidget(buildVideoPage());
    return m_videoPage;
}

QWidget *QuickView::buildVideoPage() {
    m_videoPage = new QWidget(this);
    m_videoPage->setObjectName(QStringLiteral("quickViewVideoPage"));

    m_videoSurface = m_mediaEngine->videoSurface();
    if (!m_videoSurface)
        m_videoSurface = new QWidget(m_videoPage);
    else
        m_videoSurface->setParent(m_videoPage);
    m_videoSurface->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoSurface->setMinimumHeight(120);
    m_videoSurface->installEventFilter(this); // reposition the overlay on resize

    // Floating metadata panel, parented to the video widget so it hovers on top.
    m_videoInfoOverlay = new QLabel(m_videoSurface);
    // See the image overlay: themed by object name in the .qss files.
    m_videoInfoOverlay->setObjectName(QStringLiteral("quickViewInfoOverlay"));
    m_videoInfoOverlay->setTextFormat(Qt::RichText);
    m_videoInfoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_videoInfoOverlay->hide();

    // Transient banner for things the user has to be told but that do not kill
    // the preview -- currently only a seek the decoder could not carry out.
    // Same object name, so the three themes style it with the info panel.
    m_videoNotice = new QLabel(m_videoSurface);
    m_videoNotice->setObjectName(QStringLiteral("quickViewInfoOverlay"));
    m_videoNotice->setWordWrap(true);
    m_videoNotice->setAlignment(Qt::AlignCenter);
    m_videoNotice->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_videoNotice->hide();
    m_videoNoticeTimer = new QTimer(this);
    m_videoNoticeTimer->setSingleShot(true);
    m_videoNoticeTimer->setInterval(7000);
    connect(m_videoNoticeTimer, &QTimer::timeout, this, [this]() { m_videoNotice->hide(); });

    // Control bar: play/pause, speed, progress, volume (muted by default), info.
    m_playButton = new QPushButton(tr("Play"), m_videoPage);
    // Pin a fixed width so toggling the label between "Play"/"Pause" (whose
    // translations differ in width, e.g. 播放/暂停) doesn't resize the button and
    // jitter the whole control row. Derive it from the metrics of both strings so
    // any language fits, rather than hardcoding a language-specific pixel value.
    {
        const QFontMetrics fm = m_playButton->fontMetrics();
        const int textWidth =
            qMax(fm.horizontalAdvance(tr("Play")), fm.horizontalAdvance(tr("Pause")));
        m_playButton->setFixedWidth(textWidth + 24); // + padding for button chrome
    }
    connect(m_playButton, &QPushButton::clicked, this, [this]() {
        m_mediaEngine->playPause();
        // Reflect the resulting state; playPause is async so query after a beat.
        QTimer::singleShot(50, this, [this]() {
            m_playButton->setText(
                (m_mediaEngine->paused() || m_mediaEngine->ended()) ? tr("Play")
                                                                    : tr("Pause"));
        });
    });

    m_speedCombo = new QComboBox(m_videoPage);
    // Use a QListView popup so the QSS `::item` colours apply (see m_textEncoding).
    m_speedCombo->setView(new QListView(m_speedCombo));
    // Drop the combo's outer frame so it reads as a flat control alongside the
    // play button instead of drawing an extra boxed outline. Keep the drop-down
    // sub-control (arrow) untouched so it stays a usable dropdown.
    m_speedCombo->setStyleSheet(
        "QComboBox { border: none; padding: 2px 4px; }");
    m_speedCombo->addItem(tr("1x"), 1.0);
    m_speedCombo->addItem(tr("1.5x"), 1.5);
    m_speedCombo->addItem(tr("2x"), 2.0);
    m_speedCombo->addItem(tr("3x"), 3.0);
    connect(m_speedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const double speed = m_speedCombo->itemData(index).toDouble();
                m_mediaEngine->setSpeed(speed);
                m_settings.setVideoSpeed(speed); // persist for later previews
            });

    // SeekSlider (not a plain QSlider) so clicking anywhere on the bar jumps
    // there, as in every other player.
    m_progressSlider = new SeekSlider(Qt::Horizontal, m_videoPage);
    m_progressSlider->setObjectName(QStringLiteral("quickViewVideoSeek"));
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setMinimumWidth(kMinSeekWidth);
    m_progressSlider->setToolTip(tr("Seek"));
    connect(m_progressSlider, &QSlider::sliderPressed, this, [this]() { m_seeking = true; });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this]() {
        m_mediaEngine->seekFraction(m_progressSlider->value() / 1000.0);
        m_seeking = false;
    });

    // Mute toggle: checkable, checked == muted. The icon reflects the state so a
    // muted clip reads as muted at a glance.
    m_muteButton = new QPushButton(m_videoPage);
    m_muteButton->setObjectName(QStringLiteral("quickViewVideoMute"));
    m_muteButton->setCheckable(true);
    m_muteButton->setToolTip(tr("Mute / unmute"));
    // Bump the icon size so the speaker glyph is proportionate to the button
    // instead of a tiny centred dot. This applies to every icon set on the
    // button (initial, toggle, and showFile), so it only needs setting once.
    m_muteButton->setIconSize(QSize(18, 18));
    m_muteButton->setChecked(m_settings.videoMuted());
    auto syncMuteIcon = [this]() { refreshMediaControlIcons(); };
    syncMuteIcon();
    connect(m_muteButton, &QPushButton::toggled, this, [this, syncMuteIcon](bool muted) {
        m_mediaEngine->setMute(muted);
        m_settings.setVideoMuted(muted);
        syncMuteIcon();
    });

    auto *volumeLabel = new QLabel(tr("Vol"), m_videoPage);
    m_volumeSlider = new QSlider(Qt::Horizontal, m_videoPage);
    m_volumeSlider->setObjectName(QStringLiteral("quickViewVideoVolume"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(m_settings.videoVolume());
    // Fixed-width meant the seek bar absorbed every pixel a narrow pane took
    // away, until it was too short to seek with -- the one control that has to
    // stay usable. Volume gives way first now: it may shrink to
    // kMinVolumeWidth, while the seek bar stops at kMinSeekWidth.
    m_volumeSlider->setMinimumWidth(kMinVolumeWidth);
    m_volumeSlider->setMaximumWidth(90);
    m_volumeSlider->setToolTip(tr("Volume"));
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_mediaEngine->setVolume(value);
        m_settings.setVideoVolume(value); // persist for later previews
        // Dragging the volume up is an intent to hear it: lift the mute.
        if (value > 0 && m_muteButton->isChecked())
            m_muteButton->setChecked(false); // its toggle handler unmutes + persists
    });

    // A toolbar above the video, matching the image page's. "Show info" lives
    // here rather than down among the transport controls: it is a view option,
    // like the image page's, not something you reach for mid-playback -- and
    // the transport row is exactly where width runs out first.
    auto *videoToolbar = new QToolBar(m_videoPage);
    // Quarter turns, matching the image page's pair. Clips shot on a phone come
    // out sideways often enough that this is the one image-page tool a video
    // genuinely wants -- zoom is not offered, since without panning a zoomed
    // video is half a control.
    videoToolbar->addAction(tr("Rotate Left"), this, [this]() { rotateVideoBy(-90); });
    videoToolbar->addAction(tr("Rotate Right"), this, [this]() { rotateVideoBy(90); });
    auto *videoSpacer = new QWidget(videoToolbar);
    videoSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    videoToolbar->addWidget(videoSpacer);

    m_videoInfoCheck = new QCheckBox(tr("Show info"), videoToolbar);
    m_videoInfoCheck->setToolTip(tr("Overlay basic video information"));
    videoToolbar->addWidget(m_videoInfoCheck);
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
    controls->addWidget(m_muteButton);
    controls->addWidget(volumeLabel);
    controls->addWidget(m_volumeSlider);

    auto *layout = new QVBoxLayout(m_videoPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(videoToolbar);
    layout->addWidget(m_videoSurface, 1);
    layout->addLayout(controls);

    // Poll the playback position to advance the progress slider and refresh the
    // info overlay while a clip plays.
    m_videoTimer = new QTimer(this);
    m_videoTimer->setInterval(250);
    connect(m_videoTimer, &QTimer::timeout, this, [this]() {
        // Keep the play/pause label in sync with the core: a clip that's paused
        // or sitting at EOF shows "Play" (clicking replays/resumes).
        m_playButton->setText(
            (m_mediaEngine->paused() || m_mediaEngine->ended()) ? tr("Play")
                                                                : tr("Pause"));
        if (m_seeking)
            return;
        // A backend that cannot say how long the clip is cannot be seeked
        // either -- it clamps every request to the length it wrongly believes
        // in. Leaving the bar live would let the user drag it and be thrown
        // somewhere else entirely, so it goes dead instead, once, with a
        // notice saying why.
        if (m_mediaEngine->durationIsUnknown()) {
            applyUnknownDuration();
        } else if (const double dur = m_mediaEngine->durationSeconds(); dur > 0.0) {
            const double frac = m_mediaEngine->positionSeconds() / dur;
            m_progressSlider->setValue(qBound(0, static_cast<int>(frac * 1000.0), 1000));
        }
        if (m_videoInfoOverlay->isVisible())
            updateVideoInfoOverlay();
    });

    return m_videoPage;
}

void QuickView::refreshPhosphor() {
    // Called when the theme, or the "tint images" toggle, changes. Every
    // preview holds a bitmap (or a scene) that was recoloured -- or left alone
    // -- when it was produced, so none of them notice a settings change on
    // their own; the pane just keeps showing the old treatment until the file
    // is opened again. Each surface is re-derived here from what it kept.
    const QColor tint = fc::previewTint();

    // Image: the source image is untouched, so re-running the scale step is
    // the whole job.
    if (!m_originalImage.isNull())
        applyImageScale();

    // Audio cover: kept as decoded, for exactly this reason.
    if (!m_audioCoverSource.isNull() && m_audioCover) {
        m_audioCover->setPixmap(fc::tintedPixmap(
            m_audioCoverSource.scaled(m_audioCover->size(), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation),
            tint));
    }

    // PDF: the page bitmaps were tinted as they came out of Poppler. Marking
    // every page un-rendered makes renderVisiblePdfPages() redraw the on-screen
    // ones (and only those) from the document.
    if (m_pdfDoc) {
        for (int i = 0; i < m_pdfRenderedWidth.size(); ++i)
            m_pdfRenderedWidth[i] = -1;
        renderVisiblePdfPages();
    }

    // Slides: the effect is attached per built page, so it can be swapped in
    // place -- no need to rebuild the item trees.
    for (int i = 0; i < m_slidePageItems.size(); ++i) {
        QGraphicsItem *page = m_slidePageItems.at(i);
        if (!page || !m_slideBuilt.value(i))
            continue;
        page->setGraphicsEffect(tint.isValid() ? new ttc::PhosphorEffect(tint) : nullptr);
    }

    applyVideoPhosphor();
    refreshMediaControlIcons();
}

void QuickView::applyVideoPhosphor() {
    if (!m_mediaEngine)
        return;
    VideoEffectSettings settings;
    settings.tint = fc::previewTint();
    settings.pixelBlock = fc::contentPixelBlock();
    m_mediaEngine->setVideoEffect(settings);
}

QIcon QuickView::mediaIcon(QStyle::StandardPixmap standardPixmap) const {
    return IconCache::instance().themedIcon(style()->standardIcon(standardPixmap));
}

void QuickView::refreshMediaControlIcons() {
    if (m_muteButton) {
        m_muteButton->setIcon(mediaIcon(
            m_muteButton->isChecked() ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    }
    if (!m_audioPrevButton)
        return;

    m_audioPrevButton->setIcon(mediaIcon(QStyle::SP_MediaSkipBackward));
    m_audioNextButton->setIcon(mediaIcon(QStyle::SP_MediaSkipForward));
    m_audioMuteButton->setIcon(mediaIcon(
        m_audioMuteButton->isChecked() ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    const bool playing =
        m_mediaEngine && m_mediaEngine->currentKind() == MediaKind::Audio &&
        !(m_mediaEngine->paused() || m_mediaEngine->ended());
    m_audioPlayButton->setIcon(
        mediaIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void QuickView::positionVideoInfoOverlay() {
    if (m_videoNotice && m_videoNotice->isVisible()) {
        // Wrap to the surface rather than growing off the edge: the notice is a
        // sentence, not the info panel's short lines.
        const int available = qMax(120, m_videoSurface->width() - 32);
        m_videoNotice->setFixedWidth(available);
        m_videoNotice->adjustSize();
        m_videoNotice->move(qMax(8, (m_videoSurface->width() - m_videoNotice->width()) / 2),
                            qMax(8, m_videoSurface->height() - m_videoNotice->height() - 16));
        m_videoNotice->raise();
    }
    if (!m_videoInfoOverlay->isVisible())
        return;
    m_videoInfoOverlay->adjustSize();
    const int vw = m_videoSurface->width();
    const int x = qMax(8, vw - m_videoInfoOverlay->width() - 8);
    m_videoInfoOverlay->move(x, 8);
    m_videoInfoOverlay->raise();
}

void QuickView::applyUnknownDuration() {
    // A backend that cannot say how long the clip is cannot be seeked either:
    // it clamps every request to the length it wrongly believes in (measured --
    // a request for 480 s returned S_OK and landed at 7.47). Leaving the bar
    // live would let the user drag it and be thrown somewhere else entirely.
    if (!m_progressSlider || !m_progressSlider->isEnabled())
        return;
    m_progressSlider->setEnabled(false);
    m_progressSlider->setValue(0);
    m_progressSlider->setToolTip(
        tr("This file does not say how long it is, so it cannot be seeked."));
    showVideoNotice(tr("This file does not record its own length, so the position bar "
                       "and seeking are unavailable. Playback is unaffected."));
}

void QuickView::showVideoNotice(const QString &text) {
    if (!m_videoNotice)
        return;
    m_videoNotice->setText(text);
    m_videoNotice->show();
    positionVideoInfoOverlay();
    m_videoNoticeTimer->start();
}

void QuickView::updateVideoInfoOverlay() {
    const double dur = m_mediaEngine->durationSeconds();
    const int totalSec = static_cast<int>(dur + 0.5);
    const QString duration = QString("%1:%2:%3")
                                 .arg(totalSec / 3600, 2, 10, QChar('0'))
                                 .arg((totalSec % 3600) / 60, 2, 10, QChar('0'))
                                 .arg(totalSec % 60, 2, 10, QChar('0'));
    const QSize videoSize = m_mediaEngine->currentVideoSize();
    const int w = videoSize.width();
    const int h = videoSize.height();
    const QString codec = m_mediaEngine->videoCodec();

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
    if (!m_mediaEngine || m_mediaEngine->currentKind() != MediaKind::Video ||
        m_mediaEngine->currentSource().path.isEmpty())
        return;
    if (m_videoTimer)
        m_videoTimer->stop();
    m_mediaEngine->stop();
    m_videoInfoOverlay->hide();
}

namespace {
// Formats a duration in seconds as M:SS (or H:MM:SS past an hour), for the
// audio transport's elapsed/total labels.
QString formatClock(double seconds) {
    if (seconds < 0.0 || seconds != seconds) // negative or NaN
        seconds = 0.0;
    const int total = static_cast<int>(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QString("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}
} // namespace

QWidget *QuickView::ensureAudioPage() {
    if (!m_audioPage)
        m_stack->addWidget(buildAudioPage());
    return m_audioPage;
}

QWidget *QuickView::buildAudioPage() {
    m_audioPage = new QWidget(this);
    m_audioPage->setObjectName(QStringLiteral("quickViewAudioPage"));

    // Left column: cover art with a placeholder when the file carries none.
    m_audioCover = new QLabel(m_audioPage);
    m_audioCover->setFixedSize(220, 220);
    m_audioCover->setAlignment(Qt::AlignCenter);
    m_audioCover->setStyleSheet(
        "QLabel { background: rgba(0,0,0,20); border-radius: 6px; }");

    // Right column: title + a metadata block + scrollable lyrics.
    m_audioTitle = new QLabel(m_audioPage);
    m_audioTitle->setTextFormat(Qt::RichText);
    m_audioTitle->setWordWrap(true);
    QFont titleFont = m_audioTitle->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
    titleFont.setBold(true);
    m_audioTitle->setFont(titleFont);

    m_audioMeta = new QLabel(m_audioPage);
    m_audioMeta->setTextFormat(Qt::RichText);
    m_audioMeta->setWordWrap(true);
    m_audioMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_audioLyrics = new QTextBrowser(m_audioPage);
    m_audioLyrics->setPlaceholderText(tr("No embedded lyrics."));

    auto *rightCol = new QVBoxLayout();
    rightCol->setContentsMargins(0, 0, 0, 0);
    rightCol->addWidget(m_audioTitle);
    rightCol->addWidget(m_audioMeta);
    rightCol->addWidget(m_audioLyrics, 1);

    auto *topRow = new QHBoxLayout();
    topRow->setContentsMargins(8, 8, 8, 4);
    topRow->setSpacing(12);
    auto *coverCol = new QVBoxLayout();
    coverCol->addWidget(m_audioCover, 0, Qt::AlignTop);
    coverCol->addStretch(1);
    topRow->addLayout(coverCol);
    topRow->addLayout(rightCol, 1);

    // Transport row: prev, play/pause, next, elapsed, seek, total.
    m_audioPrevButton = new QPushButton(m_audioPage);
    m_audioPrevButton->setToolTip(tr("Previous track"));
    connect(m_audioPrevButton, &QPushButton::clicked, this,
            [this]() { showPrevSibling(); });

    m_audioPlayButton = new QPushButton(m_audioPage);
    m_audioPlayButton->setToolTip(tr("Play / pause"));
    connect(m_audioPlayButton, &QPushButton::clicked, this, [this]() {
        m_mediaEngine->playPause();
        QTimer::singleShot(50, this, [this]() { updateAudioTransport(); });
    });

    m_audioNextButton = new QPushButton(m_audioPage);
    m_audioNextButton->setToolTip(tr("Next track"));
    connect(m_audioNextButton, &QPushButton::clicked, this,
            [this]() { showNextSibling(); });

    m_audioElapsed = new QLabel(QStringLiteral("0:00"), m_audioPage);
    m_audioTotal = new QLabel(QStringLiteral("0:00"), m_audioPage);

    m_audioSeek = new SeekSlider(Qt::Horizontal, m_audioPage); // click-to-seek, as in the video page
    m_audioSeek->setRange(0, 1000);
    m_audioSeek->setToolTip(tr("Seek"));
    connect(m_audioSeek, &QSlider::sliderPressed, this,
            [this]() { m_audioSeeking = true; });
    connect(m_audioSeek, &QSlider::sliderReleased, this, [this]() {
        m_mediaEngine->seekFraction(m_audioSeek->value() / 1000.0);
        m_audioSeeking = false;
    });

    // Mute toggle + volume slider, mirroring the video preview's controls but
    // backed by the independent audio/* settings.
    m_audioMuteButton = new QPushButton(m_audioPage);
    m_audioMuteButton->setObjectName(QStringLiteral("quickViewAudioMute"));
    m_audioMuteButton->setCheckable(true);
    m_audioMuteButton->setToolTip(tr("Mute / unmute"));
    m_audioMuteButton->setIconSize(QSize(18, 18));
    m_audioMuteButton->setChecked(m_settings.audioMuted());
    auto syncAudioMuteIcon = [this]() { refreshMediaControlIcons(); };
    syncAudioMuteIcon();
    connect(m_audioMuteButton, &QPushButton::toggled, this,
            [this, syncAudioMuteIcon](bool muted) {
                m_mediaEngine->setMute(muted);
                m_settings.setAudioMuted(muted);
                syncAudioMuteIcon();
            });

    auto *audioVolumeLabel = new QLabel(tr("Vol"), m_audioPage);
    m_audioVolumeSlider = new QSlider(Qt::Horizontal, m_audioPage);
    m_audioVolumeSlider->setObjectName(QStringLiteral("quickViewAudioVolume"));
    m_audioVolumeSlider->setRange(0, 100);
    m_audioVolumeSlider->setValue(m_settings.audioVolume());
    m_audioVolumeSlider->setFixedWidth(90);
    m_audioVolumeSlider->setToolTip(tr("Volume"));
    connect(m_audioVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_mediaEngine->setVolume(value);
        m_settings.setAudioVolume(value); // persist for later previews
        // Dragging the volume up is an intent to hear it: lift the mute.
        if (value > 0 && m_audioMuteButton->isChecked())
            m_audioMuteButton->setChecked(false); // its toggle handler unmutes + persists
    });

    auto *transport = new QHBoxLayout();
    transport->setContentsMargins(8, 4, 8, 8);
    transport->addWidget(m_audioPrevButton);
    transport->addWidget(m_audioPlayButton);
    transport->addWidget(m_audioNextButton);
    transport->addWidget(m_audioElapsed);
    transport->addWidget(m_audioSeek, 1);
    transport->addWidget(m_audioTotal);
    transport->addWidget(m_audioMuteButton);
    transport->addWidget(audioVolumeLabel);
    transport->addWidget(m_audioVolumeSlider);

    auto *layout = new QVBoxLayout(m_audioPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(topRow, 1);
    layout->addLayout(transport);

    // Poll the playback position to advance the seek slider and keep the
    // play/pause icon in sync while a track plays.
    m_audioTimer = new QTimer(this);
    m_audioTimer->setInterval(250);
    connect(m_audioTimer, &QTimer::timeout, this, [this]() { updateAudioTransport(); });

    refreshMediaControlIcons();
    return m_audioPage;
}

void QuickView::updateAudioTransport() {
    if (!m_mediaEngine)
        return;
    refreshMediaControlIcons();
    if (m_audioSeeking)
        return;
    const double dur = m_mediaEngine->durationSeconds();
    const double pos = m_mediaEngine->positionSeconds();
    if (dur > 0.0) {
        const double frac = pos / dur;
        m_audioSeek->setValue(qBound(0, static_cast<int>(frac * 1000.0), 1000));
        m_audioTotal->setText(formatClock(dur));
    }
    m_audioElapsed->setText(formatClock(pos));
}

void QuickView::loadAudioSiblings() {
    const QDir dir(QFileInfo(m_audioPath).absolutePath());
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    m_audioSiblings.clear();
    for (const QFileInfo &fi : entries)
        if (isAudio(fi.absoluteFilePath()))
            m_audioSiblings.append(fi.absoluteFilePath());
    m_audioSiblingIndex =
        m_audioSiblings.indexOf(QFileInfo(m_audioPath).absoluteFilePath());
}

void QuickView::showAudio(const QString &path) {
    m_infoOverlay->hide(); // image overlay belongs to another page

    // Re-selecting the exact track that's already playing is a no-op.
    if (path == m_audioPath && m_stack->currentWidget() == m_audioPage)
        return;
    m_audioPath = path;
    loadAudioSiblings();

    // Metadata: the hand-rolled ID3 reader (mp3 and friends) first; fall back to
    // libmpv's demuxer metadata for formats it doesn't cover (ogg/flac/…).
    AudioTags tags = Id3Reader::read(path);

    const QFileInfo fi(path);
    QString title = tags.title;
    QString artist = tags.artist;
    QString album = tags.album;
    if (title.isEmpty())
        title = fi.completeBaseName();

    // Cover art.
    QPixmap cover;
    if (tags.hasCover())
        cover.loadFromData(tags.coverData);
    m_audioCoverSource = cover; // kept undyed so refreshPhosphor() can redo it
    if (!cover.isNull()) {
        m_audioCover->setPixmap(
            fc::tintedPixmap(cover.scaled(m_audioCover->size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation),
                             fc::previewTint()));
    } else {
        m_audioCover->setPixmap(
            style()
                ->standardIcon(QStyle::SP_MediaVolume)
                .pixmap(96, 96));
    }

    m_audioTitle->setText(title.toHtmlEscaped());

    // Fill in missing basics from mpv once the file is loaded below; build the
    // metadata block from what we have now and refresh it after a short delay.
    auto buildMeta = [this](const QString &artist, const QString &album,
                            const AudioTags &t) {
        QStringList rows;
        auto add = [&rows](const QString &label, const QString &value) {
            if (!value.trimmed().isEmpty())
                rows << QString("<b>%1:</b> %2").arg(label, value.toHtmlEscaped());
        };
        add(tr("Artist"), artist);
        add(tr("Album"), album);
        add(tr("Album Artist"), t.albumArtist);
        add(tr("Year"), t.year);
        add(tr("Genre"), t.genre);
        add(tr("Track"), t.track);
        add(tr("Composer"), t.composer);
        m_audioMeta->setText(rows.join(QStringLiteral("<br>")));
    };
    buildMeta(artist, album, tags);

    // Lyrics (embedded USLT only; sidecar/online lyrics are out of scope).
    if (!tags.lyrics.isEmpty())
        m_audioLyrics->setPlainText(tags.lyrics);
    else
        m_audioLyrics->clear();

    // Apply the persisted audio volume/mute (independent of the video preview,
    // so the audio player defaults to un-muted) and sync the transport controls.
    const int audioVol = m_settings.audioVolume();
    const bool audioMute = m_settings.audioMuted();
    m_audioVolumeSlider->blockSignals(true);
    m_audioVolumeSlider->setValue(audioVol);
    m_audioVolumeSlider->blockSignals(false);
    m_audioMuteButton->blockSignals(true);
    m_audioMuteButton->setChecked(audioMute);
    refreshMediaControlIcons();
    m_audioMuteButton->blockSignals(false);
    m_mediaEngine->setVolume(audioVol);
    m_mediaEngine->setMute(audioMute);

    m_audioSeek->setValue(0);
    m_audioElapsed->setText(QStringLiteral("0:00"));
    m_audioTotal->setText(QStringLiteral("0:00"));
    m_mediaEngine->load(MediaSource{path, {}, false}, MediaKind::Audio);
    if (m_mediaEngine->state() == MediaState::Failed)
        return;
    m_stack->setCurrentWidget(m_audioPage);
    releaseHiddenDocumentPages(m_audioPage);
    m_audioTimer->start();

    // If the ID3 reader found no artist/album (non-mp3 formats), pull them from
    // mpv's demuxer metadata once it has opened the file.
    if (tags.artist.isEmpty() || tags.album.isEmpty() || tags.title.isEmpty()) {
        QTimer::singleShot(300, this, [this, path, artist, album, tags]() {
            if (m_audioPath != path) // user moved on
                return;
            QString a =
                artist.isEmpty() ? m_mediaEngine->metadataValue(QStringLiteral("artist"))
                                 : artist;
            QString al =
                album.isEmpty() ? m_mediaEngine->metadataValue(QStringLiteral("album"))
                                : album;
            if (m_audioTitle->text().isEmpty() || tags.title.isEmpty()) {
                const QString mt = m_mediaEngine->metadataValue(QStringLiteral("title"));
                if (!mt.isEmpty())
                    m_audioTitle->setText(mt.toHtmlEscaped());
            }
            QStringList rows;
            auto add = [&rows](const QString &label, const QString &value) {
                if (!value.trimmed().isEmpty())
                    rows << QString("<b>%1:</b> %2").arg(label, value.toHtmlEscaped());
            };
            add(tr("Artist"), a);
            add(tr("Album"), al);
            add(tr("Album Artist"), tags.albumArtist);
            add(tr("Year"), tags.year);
            add(tr("Genre"), tags.genre);
            add(tr("Track"), tags.track);
            add(tr("Composer"), tags.composer);
            if (!rows.isEmpty())
                m_audioMeta->setText(rows.join(QStringLiteral("<br>")));
        });
    }
}

void QuickView::stopAudio() {
    if (!m_mediaEngine || m_mediaEngine->currentKind() != MediaKind::Audio ||
        m_mediaEngine->currentSource().path.isEmpty())
        return;
    if (m_audioTimer)
        m_audioTimer->stop();
    m_mediaEngine->stop();
    m_audioPath.clear();
}

void QuickView::stopPlayback() {
    stopVideo();
    stopAudio();
}

QWidget *QuickView::buildMarkdownPage() {
    // A read-only rich-text browser. Qt renders Markdown through its bundled
    // MD4C parser (QTextDocument::setMarkdown), so no external md4c is linked.
    // Open links in the user's browser rather than trying to navigate in-panel.
    m_markdown = new QTextBrowser(this);
    m_markdown->setOpenExternalLinks(true);
    m_markdown->document()->setDefaultStyleSheet(kMarkdownDefaultCss);
    return m_markdown;
}

void QuickView::loadMarkdownAsync(const QString &path) {
    // Supersede any in-flight render so a fast scroll through several .md files
    // only ever installs the newest one.
    ++m_markdownGen;
    const int gen = m_markdownGen;

    // Capture the browser's font + current width so the off-thread layout matches
    // the final on-screen layout -- installing the ready-made document then costs
    // no extra re-layout on the GUI thread.
    const QFont font = m_markdown->font();
    const int width = qMax(200, m_markdown->viewport()->width());

    auto *watcher = new QFutureWatcher<QTextDocument *>(this);
    connect(watcher, &QFutureWatcher<QTextDocument *>::finished, this,
            [this, watcher, gen]() {
                watcher->deleteLater();
                QTextDocument *doc = watcher->result();
                if (!doc)
                    return; // unreadable file; leave the current preview in place
                if (gen != m_markdownGen) {
                    delete doc; // a newer selection superseded this render
                    return;
                }
                // setDocument doesn't take ownership, so parent the document to the
                // browser: the previous child document is deleted on the next swap.
                doc->setParent(m_markdown);
                m_markdown->setDocument(doc);
                revealStaticPage(m_markdown);
            });
    watcher->setFuture(QtConcurrent::run([path, font, width]() -> QTextDocument * {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return nullptr;
        // Cap the read so a pathologically large .md can't stall the render.
        const QByteArray data = f.read(kMarkdownMaxBytes);
        auto *doc = new QTextDocument;
        doc->setDefaultFont(font);
        doc->setDefaultStyleSheet(kMarkdownDefaultCss);
        // QTextDocument::setMarkdown wants the GitHub dialect (tables, task lists).
        doc->setMarkdown(QString::fromUtf8(data), QTextDocument::MarkdownDialectGitHub);
        doc->setTextWidth(width); // force the expensive layout here, off the GUI thread
        // The document was created on this worker thread; hand it to the GUI thread
        // so setParent()/setDocument() there are legal.
        doc->moveToThread(qApp->thread());
        return doc;
    }));
}

QString QuickView::fitImagesToWidth(const QString &html, int maxWidth) const {
    if (maxWidth <= 0)
        return html;
    static const QRegularExpression imgRe(QStringLiteral("<img\\b[^>]*>"));
    static const QRegularExpression srcRe(
        QStringLiteral("src=\"data:image/[^;]+;base64,([^\"]+)\""));
    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = imgRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        QString tag = m.captured(0);
        // Only size tags that carry a data: URI and don't already declare a width.
        if (!tag.contains(QStringLiteral("width="))) {
            const QRegularExpressionMatch sm = srcRe.match(tag);
            if (sm.hasMatch()) {
                QByteArray data = QByteArray::fromBase64(sm.captured(1).toLatin1());
                QBuffer buf(&data);
                buf.open(QIODevice::ReadOnly);
                QImageReader reader(&buf);
                const QSize sz = reader.size();
                if (sz.isValid() && sz.width() > maxWidth) {
                    const int w = maxWidth;
                    const int h = int(static_cast<qint64>(sz.height()) * maxWidth / sz.width());
                    int insertAt = tag.lastIndexOf(QLatin1Char('>'));
                    if (insertAt > 0 && tag.at(insertAt - 1) == QLatin1Char('/'))
                        --insertAt;
                    tag.insert(insertAt, QStringLiteral(" width=\"%1\" height=\"%2\"").arg(w).arg(h));
                }
            }
        }
        out += tag;
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

QWidget *QuickView::buildOfficeTablePage() {
    // One grid per worksheet, selected via a bottom tab bar (Excel-style). The
    // tabs are populated per file in populateSheets(); the bar hides itself for a
    // single-sheet workbook so a lone sheet reads as a plain grid.
    m_officeTabs = new QTabWidget(this);
    m_officeTabs->setObjectName(QStringLiteral("quickViewOfficePage"));
    m_officeTabs->setTabPosition(QTabWidget::South);
    m_officeTabs->setDocumentMode(true);
    QFont font = m_officeTabs->font();
    if (!m_contentFontFamily.isEmpty())
        font.setFamily(m_contentFontFamily);
    if (m_contentFontSize > 0)
        font.setPointSize(m_contentFontSize);
    m_officeTabs->setFont(font);
    return m_officeTabs;
}

// The QTableWidget behind the currently selected worksheet tab (null when the
// workbook produced no sheets).
QTableWidget *QuickView::currentOfficeTable() const {
    return m_officeTabs ? qobject_cast<QTableWidget *>(m_officeTabs->currentWidget()) : nullptr;
}

QWidget *QuickView::buildEncryptedPage() {
    m_encryptedPage = new QWidget(this);
    auto *outer = new QVBoxLayout(m_encryptedPage);
    outer->addStretch(1);

    // A narrow centered column: prompt, password field, unlock button, feedback.
    auto *column = new QVBoxLayout;
    column->setSpacing(10);

    m_encryptedLabel = new QLabel(m_encryptedPage);
    m_encryptedLabel->setAlignment(Qt::AlignCenter);
    m_encryptedLabel->setWordWrap(true);
    column->addWidget(m_encryptedLabel);

    m_passwordEdit = new QLineEdit(m_encryptedPage);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Password"));
    m_passwordEdit->setFixedWidth(240);
    // Intercept Tab/Backtab so it returns focus to the file list rather than
    // advancing to the Unlock button (handled in eventFilter).
    m_passwordEdit->installEventFilter(this);
    column->addWidget(m_passwordEdit, 0, Qt::AlignHCenter);

    m_unlockButton = new QPushButton(tr("Unlock"), m_encryptedPage);
    m_unlockButton->setFixedWidth(240);
    column->addWidget(m_unlockButton, 0, Qt::AlignHCenter);

    m_encryptedFeedback = new QLabel(m_encryptedPage);
    m_encryptedFeedback->setAlignment(Qt::AlignCenter);
    m_encryptedFeedback->setWordWrap(true);
    m_encryptedFeedback->setProperty("semanticState", QStringLiteral("error"));
    column->addWidget(m_encryptedFeedback);

    outer->addLayout(column);
    outer->addStretch(1);

    // Enter in the field or the button both attempt to unlock.
    connect(m_unlockButton, &QPushButton::clicked, this, &QuickView::tryUnlock);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &QuickView::tryUnlock);
    return m_encryptedPage;
}

QWidget *QuickView::buildDownloadPage() {
    m_downloadPage = new QWidget(this);
    auto *outer = new QVBoxLayout(m_downloadPage);
    outer->addStretch(1);

    auto *column = new QVBoxLayout;
    column->setSpacing(12);

    m_downloadLabel = new QLabel(m_downloadPage);
    m_downloadLabel->setAlignment(Qt::AlignCenter);
    m_downloadLabel->setWordWrap(true);
    column->addWidget(m_downloadLabel);

    m_downloadProgress = new QProgressBar(m_downloadPage);
    m_downloadProgress->setFixedWidth(320);
    m_downloadProgress->setRange(0, 0); // indeterminate until a total is known
    column->addWidget(m_downloadProgress, 0, Qt::AlignHCenter);

    m_downloadStopButton = new QPushButton(tr("停止下载"), m_downloadPage);
    m_downloadStopButton->setFixedWidth(160);
    column->addWidget(m_downloadStopButton, 0, Qt::AlignHCenter);

    outer->addLayout(column);
    outer->addStretch(1);

    connect(m_downloadStopButton, &QPushButton::clicked, this,
            &QuickView::downloadCancelRequested);
    return m_downloadPage;
}

void QuickView::showDownloading(const QString &name) {
    cancelPendingPreviewWork();
    m_downloadLabel->setText(tr("正在下载到本地以便预览…\n%1").arg(name));
    m_downloadProgress->setRange(0, 0); // reset to indeterminate
    m_downloadProgress->setVisible(true);
    m_downloadStopButton->setVisible(true);
    m_stack->setCurrentWidget(m_downloadPage);
    releaseHiddenDocumentPages(m_downloadPage);
}

void QuickView::showPreparing(const QString &name) {
    cancelPendingPreviewWork();
    m_downloadLabel->setText(tr("Preparing preview…\n%1").arg(name));
    m_downloadProgress->setRange(0, 0); // indeterminate: an extract has no progress
    m_downloadProgress->setVisible(true);
    m_downloadStopButton->setVisible(false);
    m_stack->setCurrentWidget(m_downloadPage);
    releaseHiddenDocumentPages(m_downloadPage);
}

void QuickView::setDownloadProgress(qint64 done, qint64 total) {
    if (!m_downloadProgress)
        return;
    if (total > 0) {
        // Scale to KiB so the int range holds large files.
        m_downloadProgress->setRange(0, static_cast<int>(total / 1024 + 1));
        m_downloadProgress->setValue(static_cast<int>(done / 1024));
    } else {
        m_downloadProgress->setRange(0, 0); // unknown size: keep it indeterminate
    }
}

void QuickView::showDownloadCancelled(const QString &name) {
    cancelPendingPreviewWork();
    m_downloadLabel->setText(tr("已取消预览：本文件的预览下载被用户停止。\n%1").arg(name));
    m_downloadProgress->setVisible(false);
    m_downloadStopButton->setVisible(false);
    m_stack->setCurrentWidget(m_downloadPage);
    releaseHiddenDocumentPages(m_downloadPage);
}

void QuickView::tryUnlock() {
    if (m_encryptedPath.isEmpty())
        return;
    const QString password = m_passwordEdit->text();
    if (password.isEmpty()) {
        m_encryptedFeedback->setText(tr("Enter a password."));
        return;
    }
    if (m_encryptedKind == EncryptedKind::Office) {
        // office_oxide decrypts in-process and renders directly; renderOffice()
        // shows the document on success or refreshes feedback on a wrong password.
        // Unlock is an explicit user action, not a cursor sweep: render immediately and
    // cancel any pending debounced convert.
    if (m_officeConvertTimer)
        m_officeConvertTimer->stop();
    startOfficeRender(m_encryptedPath, password);
        return;
    }
    // Archive: retry the current chain level with the entered password. The load
    // runs on a worker thread; handleArchiveLoad() shows the listing on success
    // or refreshes this page's feedback on a wrong password.
    if (!m_archivePasswords.isEmpty())
        m_archivePasswords.last() = password;
    tryLoadCurrentArchive();
}

void QuickView::renderOffice(const QString &path, const QString &password) {
    // Debounce rapid file switches: a fast arrow-key sweep through a folder would
    // otherwise spawn one office_oxide process per file (the gen counter only drops
    // the stale *result*, the subprocess still ran). Stash the target and (re)start
    // the timer; the convert fires once the cursor settles on a file.
    m_pendingOfficePath = path;
    m_pendingOfficePassword = password;
    if (!m_officeConvertTimer) {
        m_officeConvertTimer = new QTimer(this);
        m_officeConvertTimer->setSingleShot(true);
        connect(m_officeConvertTimer, &QTimer::timeout, this,
                [this]() { startOfficeRender(m_pendingOfficePath, m_pendingOfficePassword); });
    }
    m_officeConvertTimer->start(150);
}

void QuickView::startOfficeRender(const QString &path, const QString &password) {
    // Run the conversion (subprocess + JSON parse -- up to a second-plus on a big
    // deck) off the GUI thread so selecting a pptx never freezes the UI. A per-call
    // gen guards against a stale result painting over a newer selection.
    //
    // Presentations load in two stages: stage 1 renders only the first few slides
    // (`svg --first N`, fast) so the preview appears almost immediately and lands on
    // slide 1; stage 2 renders the whole deck in the background and appends the rest,
    // keeping the current scroll position. Word/spreadsheet files convert once.
    const int gen = ++m_officeGen;
    m_officeShownPath.clear(); // not yet showing this file's content

    const int firstN = OfficeConverter::isPresentationFile(path) ? kFirstStageSlides : 0;

    auto *w1 = new QFutureWatcher<OfficeConverter::Result>(this);
    connect(w1, &QFutureWatcher<OfficeConverter::Result>::finished, this,
            [this, w1, gen, path, password, firstN]() {
                const OfficeConverter::Result r = w1->result();
                w1->deleteLater();
                if (gen != m_officeGen)
                    return; // a newer selection superseded this conversion
                handleOfficeResult(r, path);

                // Stage 2: a full first-stage deck (exactly N slides) likely has
                // more -- fetch the whole deck and append the remainder in the
                // background, leaving the shown slides and scroll position intact.
                if (!(r.ok && r.kind == OfficeConverter::Kind::Presentation && firstN > 0 &&
                      r.slideSvgs.size() == firstN))
                    return;
                auto *w2 = new QFutureWatcher<OfficeConverter::Result>(this);
                connect(w2, &QFutureWatcher<OfficeConverter::Result>::finished, this,
                        [this, w2, gen, path]() {
                            const OfficeConverter::Result r2 = w2->result();
                            w2->deleteLater();
                            if (gen != m_officeGen)
                                return;
                            if (r2.ok && r2.kind == OfficeConverter::Kind::Presentation &&
                                r2.slideSvgs.size() > m_slideSvgs.size())
                                appendRemainingSlides(r2.slideSvgs);
                        });
                w2->setFuture(QtConcurrent::run(
                    [path, password]() { return OfficeConverter::convert(path, password, 0); }));
            });
    w1->setFuture(QtConcurrent::run([path, password, firstN]() {
        return OfficeConverter::convert(path, password, firstN);
    }));
}

void QuickView::handleOfficeResult(const OfficeConverter::Result &r, const QString &path) {
    const QFileInfo info(path);

    if (r.ok && r.kind == OfficeConverter::Kind::Presentation && !r.slideSvgs.isEmpty()) {
        // pptx rendered as slide images: stack them in the continuous slides page.
        ensureSlidesPage();
        loadSlides(r.slideSvgs);
        revealStaticPage(m_slidesPage);
        m_officeShownPath = path; // de-dupe a spurious re-selection of this file
        return;
    }
    if (r.ok && r.kind == OfficeConverter::Kind::Document) {
        // Fit large embedded images to the preview width. Use the stack's width
        // (the actual pane width) rather than m_markdown's, which may still be
        // stale until it's shown as the current page below.
        const int avail = qMax(200, m_stack->width() - 32);
        m_markdown->setHtml(fitImagesToWidth(r.html, avail));
        revealStaticPage(m_markdown);
        m_officeShownPath = path;
        return;
    }
    if (r.ok && r.kind == OfficeConverter::Kind::Spreadsheet) {
        ensureOfficePage();
        populateSheets(r.sheets);
        revealStaticPage(m_officeTabs);
        m_officeShownPath = path;
        return;
    }

    switch (r.encryption) {
    case OfficeConverter::Encryption::NeedsPassword:
        // First encounter: show the inline field but do NOT steal focus -- the
        // page can appear merely from moving the file-list cursor. Remember what
        // held focus so Tab in the field can return there.
        m_encryptedKind = EncryptedKind::Office;
        m_encryptedPath = path;
        m_encryptedLabel->setText(
            tr("“%1” is encrypted. Enter the password to preview it:").arg(info.fileName()));
        m_encryptedFeedback->clear();
        m_passwordEdit->clear();
        m_passwordEdit->show();
        m_unlockButton->show();
        m_focusBeforeEncrypted = QApplication::focusWidget();
        m_stack->setCurrentWidget(m_encryptedPage);
        releaseHiddenDocumentPages(m_encryptedPage);
        return;
    case OfficeConverter::Encryption::WrongPassword:
        // Stay on the page and report in place; let the user retype.
        m_encryptedKind = EncryptedKind::Office;
        m_encryptedPath = path;
        m_encryptedFeedback->setText(tr("Incorrect password. Try again."));
        m_passwordEdit->selectAll();
        m_passwordEdit->setFocus();
        m_stack->setCurrentWidget(m_encryptedPage);
        releaseHiddenDocumentPages(m_encryptedPage);
        return;
    case OfficeConverter::Encryption::Unsupported:
        // No password can help (legacy .xls/.ppt): note it, hide the field.
        m_encryptedPath.clear();
        m_encryptedLabel->setText(
            tr("“%1” is encrypted in a format that can't be previewed.").arg(info.fileName()));
        m_encryptedFeedback->clear();
        m_passwordEdit->hide();
        m_unlockButton->hide();
        m_stack->setCurrentWidget(m_encryptedPage);
        releaseHiddenDocumentPages(m_encryptedPage);
        return;
    case OfficeConverter::Encryption::None:
        break;
    }

    m_info->setText(tr("Cannot preview %1:\n%2").arg(info.fileName(), r.error));
    revealStaticPage(m_info);
}

QWidget *QuickView::buildArchivePage() {
    m_archivePage = new QWidget(this);
    m_archivePage->setObjectName(QStringLiteral("quickViewArchivePage"));
    m_archiveModel = new ArchiveModel(this);

    auto *toolbar = new QToolBar(m_archivePage);
    toolbar->addAction(tr("Up"), this, &QuickView::navigateArchiveUp);
    m_archivePathLabel = new QLabel(m_archivePage);
    m_archivePathLabel->setContentsMargins(8, 0, 8, 0);
    toolbar->addWidget(m_archivePathLabel);

    // Package-metadata panel (hidden unless the root archive is a .deb/.rpm).
    // Read-only, no frame, wraps long Description lines; capped so a big control
    // file never crowds out the file tree.
    m_archiveInfoView = new QTextEdit(m_archivePage);
    m_archiveInfoView->setReadOnly(true);
    m_archiveInfoView->setFrameShape(QFrame::NoFrame);
    m_archiveInfoView->setLineWrapMode(QTextEdit::WidgetWidth);
    m_archiveInfoView->setMaximumHeight(180);
    m_archiveInfoView->hide();

    m_archiveView = new QTableView(m_archivePage);
    m_archiveView->setModel(m_archiveModel);
    m_archiveView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_archiveView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_archiveView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_archiveView->verticalHeader()->hide();
    m_archiveView->horizontalHeader()->setStretchLastSection(false);
    m_archiveView->horizontalHeader()->setSectionResizeMode(ArchiveModel::NameColumn,
                                                            QHeaderView::Stretch);
    connect(m_archiveView, &QAbstractItemView::activated, this, &QuickView::onArchiveActivated);

    auto *layout = new QVBoxLayout(m_archivePage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_archiveInfoView);
    layout->addWidget(m_archiveView, 1);
    return m_archivePage;
}

void QuickView::previewArchive(const QString &path) {
    stopVideo();
    stopAudio();
    m_infoOverlay->hide();
    // Fresh chain rooted at this archive; drop any previously extracted nesteds.
    m_archivePaths = QStringList{path};
    m_archivePasswords = QStringList{QString()};
    m_nestedDir.reset();
    m_archiveAutoDescend = true; // a fresh preview drills through single-entry chains
    tryLoadCurrentArchive();
}

void QuickView::tryLoadCurrentArchive() {
    const QString path = m_archivePaths.last();
    const QString pw = m_archivePasswords.last();
    const QFileInfo fi(path);
    const qint64 size = fi.size();
    const qint64 mtime = fi.lastModified().toSecsSinceEpoch();

    // Cache hit (same path, unchanged size/mtime, same password): populate now.
    const auto cached = m_archiveCache.constFind(path);
    if (cached != m_archiveCache.constEnd() && cached->root && cached->size == size &&
        cached->mtime == mtime && cached->passphrase == pw) {
        m_archiveModel->setTree(cached->root, path, pw);
        updateArchivePathLabel();
        setArchivePackageInfo(cached->packageInfo);
        revealStaticPage(m_archivePage);
        autoDescendArchive();
        return;
    }

    // Supersede any in-flight load, then list on a worker thread so a big
    // solid/streaming archive (tar.bz2, 7z solid) never freezes the UI.
    ++m_archiveGen;
    const int gen = m_archiveGen;
    if (m_archiveCancel)
        m_archiveCancel->store(true);
    m_archiveCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_archiveCancel;

    auto *watcher = new QFutureWatcher<ArchiveLoadResult>(this);
    connect(watcher, &QFutureWatcher<ArchiveLoadResult>::finished, this,
            [this, watcher, gen, path, pw, size, mtime]() {
                watcher->deleteLater();
                if (gen != m_archiveGen)
                    return; // a newer selection superseded this load
                handleArchiveLoad(watcher->result(), path, pw, size, mtime);
            });
    watcher->setFuture(QtConcurrent::run([path, pw, cancel]() {
        ArchiveLoadResult r;
        r.root = ArchiveHandler::buildTree(path, pw, &r.status, &r.err, cancel.get());
        // Read .deb/.rpm metadata here (off the GUI thread) so the extra I/O and
        // decompression never hitches the preview; PackageInfo returns empty for
        // anything else.
        if (r.status == ArchiveHandler::Status::Ok && r.root)
            r.packageInfo = PackageInfo::forPackage(path);
        return r;
    }));
}

void QuickView::handleArchiveLoad(const ArchiveLoadResult &r, const QString &path,
                                  const QString &pw, qint64 size, qint64 mtime) {
    const QString name = QFileInfo(path).fileName();

    if (r.status == ArchiveHandler::Status::Ok && r.root) {
        // Cache the tree (bounded, FIFO eviction) so re-visits and "Up" are instant.
        m_archiveCache.insert(path, {r.root, pw, size, mtime, r.packageInfo});
        m_archiveCacheOrder.removeAll(path);
        m_archiveCacheOrder.append(path);
        constexpr int kMaxCachedArchives = 8;
        while (m_archiveCacheOrder.size() > kMaxCachedArchives)
            m_archiveCache.remove(m_archiveCacheOrder.takeFirst());

        m_archiveModel->setTree(r.root, path, pw);
        updateArchivePathLabel();
        setArchivePackageInfo(r.packageInfo);
        revealStaticPage(m_archivePage);
        autoDescendArchive();
        return;
    }

    switch (r.status) {
    case ArchiveHandler::Status::NeedPassword: {
        // ZIP/RAR5 decrypt via libarchive+nettle; 7z (incl. header-encrypted
        // -mhe) via the in-process SevenZipReader. All prompt for a password
        // here. (Encrypted RAR4, which nothing here can decrypt, still degrades
        // gracefully: the prompt leads to an "unsupported" note on submit.)
        // Gate the preview behind the inline password page (same UI as office).
        // Don't steal focus: the page can appear just from cursor movement.
        m_encryptedKind = EncryptedKind::Archive;
        m_encryptedPath = path;
        m_encryptedLabel->setText(
            tr("“%1” is encrypted. Enter the password to preview it:").arg(name));
        m_encryptedFeedback->clear();
        m_passwordEdit->clear();
        m_passwordEdit->show();
        m_unlockButton->show();
        m_focusBeforeEncrypted = QApplication::focusWidget();
        m_stack->setCurrentWidget(m_encryptedPage);
        releaseHiddenDocumentPages(m_encryptedPage);
        break;
    }
    case ArchiveHandler::Status::WrongPassword:
        m_encryptedKind = EncryptedKind::Archive;
        m_encryptedPath = path;
        m_encryptedFeedback->setText(tr("Incorrect password. Try again."));
        m_passwordEdit->selectAll();
        m_passwordEdit->setFocus();
        m_stack->setCurrentWidget(m_encryptedPage);
        releaseHiddenDocumentPages(m_encryptedPage);
        break;
    case ArchiveHandler::Status::EncryptedUnsupported:
        m_info->setText(
            tr("“%1” uses an encryption that can't be previewed.").arg(name));
        revealStaticPage(m_info);
        break;
    default:
        m_info->setText(tr("Cannot open archive: %1").arg(name));
        revealStaticPage(m_info);
        break;
    }
}

void QuickView::descendIntoNestedArchive(const QString &entryFullPath, const QString &entryName) {
    if (!m_nestedDir)
        m_nestedDir = std::make_unique<QTemporaryDir>();
    if (!m_nestedDir->isValid()) {
        m_info->setText(tr("Could not create a temporary directory."));
        revealStaticPage(m_info);
        return;
    }
    // Extract just this entry into a per-level subdir so names never collide.
    const QString sub =
        QDir(m_nestedDir->path()).filePath(QString::number(m_archivePaths.size()));
    QDir().mkpath(sub);
    QString err;
    if (!ArchiveHandler::extract(m_archivePaths.last(), {entryFullPath}, sub,
                                 m_archivePasswords.last(), &err)) {
        m_info->setText(tr("Could not extract %1: %2").arg(entryName, err));
        revealStaticPage(m_info);
        return;
    }
    const QString nested = QDir(sub).filePath(entryFullPath);
    if (!QFileInfo::exists(nested)) {
        m_info->setText(tr("Could not read the nested archive %1.").arg(entryName));
        revealStaticPage(m_info);
        return;
    }
    m_archivePaths.append(nested);
    m_archivePasswords.append(QString());
    tryLoadCurrentArchive();
}

void QuickView::autoDescendArchive() {
    // Follow a single-entry chain automatically until the level branches or ends
    // on a plain file. enterDirectory() is synchronous (loop here); a lone nested
    // archive extracts + reloads asynchronously and this routine re-runs from the
    // reload's autoDescend (the flag stays set through automatic descents).
    for (;;) {
        if (!m_archiveAutoDescend)
            return;
        const int rows = m_archiveModel->rowCount();
        // Row 0 is the ".." parent entry except at the archive root.
        const int firstReal = m_archiveModel->isAtRoot() ? 0 : 1;
        if (rows - firstReal != 1)
            return; // empty, or it branches -> stop on this level
        const auto only = m_archiveModel->nodeAt(firstReal);
        if (!only)
            return;
        if (only->isDir) {
            m_archiveModel->enterDirectory(only->fullPath);
            updateArchivePathLabel();
            continue; // keep drilling down
        }
        if (ArchiveHandler::isSupportedArchive(only->name)) {
            descendIntoNestedArchive(only->fullPath, only->name);
            return; // continues from the reload's autoDescend
        }
        return; // a single plain file: stop and show this level
    }
}

void QuickView::onArchiveActivated(const QModelIndex &index) {
    m_archiveAutoDescend = false; // the user is navigating by hand now
    if (m_archiveModel->isParentEntry(index.row())) {
        navigateArchiveUp();
        return;
    }
    const auto node = m_archiveModel->nodeAt(index.row());
    if (!node)
        return;
    if (node->isDir) {
        m_archiveModel->enterDirectory(node->fullPath);
        updateArchivePathLabel();
        return;
    }
    // A file entry that is itself an archive: extract it and descend inside.
    if (ArchiveHandler::isSupportedArchive(node->name))
        descendIntoNestedArchive(node->fullPath, node->name);
}

void QuickView::navigateArchiveUp() {
    m_archiveAutoDescend = false; // manual "Up" hands control back to the user
    if (!m_archiveModel->isAtRoot()) {
        if (m_archiveModel->navigateUp())
            updateArchivePathLabel();
        return;
    }
    // At the root of a nested archive: pop back to the parent archive.
    if (m_archivePaths.size() > 1) {
        m_archivePaths.removeLast();
        m_archivePasswords.removeLast();
        tryLoadCurrentArchive();
    }
}

void QuickView::updateArchivePathLabel() {
    // "outer.zip › inner.zip / subdir" -- the archive chain plus the path inside.
    QString label;
    for (const QString &p : m_archivePaths)
        label += QFileInfo(p).fileName() + QStringLiteral(" › ");
    label += QStringLiteral("/%1").arg(m_archiveModel->currentPath());
    m_archivePathLabel->setText(label);
}

void QuickView::setArchivePackageInfo(const QString &info) {
    if (info.isEmpty()) {
        m_archiveInfoView->clear();
        m_archiveInfoView->hide();
        return;
    }
    // Plain text keeps the control/header exactly as written (no HTML escaping
    // surprises) and inherits the theme's text colour like the rest of the UI.
    m_archiveInfoView->setPlainText(info);
    m_archiveInfoView->show();
}

// Build a read-only grid for one worksheet's TSV. office-oxide's per-sheet text
// is tab-separated: one row per line, cells split on '\t' (commas are literal, so
// no quote handling needed).
static QTableWidget *makeSheetGrid(const QString &tsv, const QFont &font, QWidget *parent) {
    auto *table = new QTableWidget(parent);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setVisible(false);
    table->verticalHeader()->setVisible(false);
    table->setFont(font);

    const QStringList lines = tsv.split(QLatin1Char('\n'));
    QVector<QStringList> rows;
    for (const QString &line : lines) {
        if (line.isEmpty() && &line == &lines.last())
            continue; // drop a trailing empty line from the final newline
        rows.append(line.split(QLatin1Char('\t')));
    }
    int cols = 0;
    for (const auto &r : rows)
        cols = qMax(cols, r.size());
    table->setRowCount(rows.size());
    table->setColumnCount(cols);
    table->verticalHeader()->setDefaultSectionSize(QFontMetrics(font).height() + 6);
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows.at(r).size(); ++c)
            table->setItem(r, c, new QTableWidgetItem(rows.at(r).at(c)));
    table->resizeColumnsToContents();
    return table;
}

void QuickView::populateSheets(const QVector<QPair<QString, QString>> &sheets) {
    // Replace the previous file's tabs. deleteLater on removed pages keeps this
    // safe even if a stale async result races in.
    while (m_officeTabs->count() > 0) {
        QWidget *w = m_officeTabs->widget(0);
        m_officeTabs->removeTab(0);
        w->deleteLater();
    }
    const QFont font = m_officeTabs->font();
    for (const auto &sheet : sheets) {
        QTableWidget *grid = makeSheetGrid(sheet.second, font, m_officeTabs);
        // A sheet with no name (legacy fallback) gets a generic label.
        const QString name = sheet.first.isEmpty()
                                 ? tr("Sheet %1").arg(m_officeTabs->count() + 1)
                                 : sheet.first;
        m_officeTabs->addTab(grid, name);
    }
    // Hide the tab strip for a single-sheet workbook so it reads as a plain grid.
    m_officeTabs->tabBar()->setVisible(sheets.size() > 1);
}

QWidget *QuickView::buildPdfPage() {
    m_pdfPage = new QWidget(this);
    m_pdfPage->setObjectName(QStringLiteral("quickViewPdfPage"));

    // A slim toolbar: zoom (fit-to-width is the implicit default), the copy-text
    // fallbacks, and the "page N / M" readout driven by scroll position.
    auto *toolbar = new QToolBar(m_pdfPage);
    toolbar->addAction(tr("Zoom In"), this, [this]() {
        if (!m_pdfDoc)
            return;
        m_pdfZoom = qBound(kPdfMinZoom, m_pdfZoom * kZoomStep, kPdfMaxZoom);
        relayoutPdfPages();
        renderVisiblePdfPages();
    });
    toolbar->addAction(tr("Zoom Out"), this, [this]() {
        if (!m_pdfDoc)
            return;
        m_pdfZoom = qBound(kPdfMinZoom, m_pdfZoom / kZoomStep, kPdfMaxZoom);
        relayoutPdfPages();
        renderVisiblePdfPages();
    });
    toolbar->addSeparator();
    toolbar->addAction(tr("Copy Page"), this,
                       [this]() { copyPdfText(CopyScope::CurrentPage); });
    toolbar->addAction(tr("Copy All"), this, [this]() { copyPdfText(CopyScope::All); });

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    m_pdfPageInfo = new QLabel(m_pdfPage);
    m_pdfPageInfo->setContentsMargins(8, 0, 8, 0);
    toolbar->addWidget(m_pdfPageInfo);

    // Pages live in one continuous scene: a bitmap background per page (kept sharp
    // by re-rendering at the zoomed resolution) with a transparent selectable text
    // layer on top. The view scrolls the scene; it does NOT scale it, so the bitmap
    // stays crisp rather than being stretched.
    m_pdfScene = new QGraphicsScene(this);
    m_pdfScene->setBackgroundBrush(palette().color(QPalette::Dark));
    m_pdfView = new QGraphicsView(m_pdfScene, m_pdfPage);
    m_pdfView->setFrameShape(QFrame::NoFrame);
    m_pdfView->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_pdfView->setDragMode(QGraphicsView::NoDrag); // let text items own the mouse
    m_pdfView->setRenderHint(QPainter::SmoothPixmapTransform, true);
    m_pdfView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Resizing the pane re-fits every page to the new width; debounce so a divider
    // drag re-fits once at the end rather than on every intermediate width.
    m_pdfView->viewport()->installEventFilter(this);

    m_pdfRelayoutTimer = new QTimer(this);
    m_pdfRelayoutTimer->setSingleShot(true);
    m_pdfRelayoutTimer->setInterval(80);
    connect(m_pdfRelayoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_pdfDoc)
            return;
        relayoutPdfPages();
        renderVisiblePdfPages();
    });

    // Scrolling reveals new pages (render them) and updates the page readout.
    connect(m_pdfView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this]() { renderVisiblePdfPages(); });

    // Ctrl+C copies whatever text is selected in the scene.
    auto *copySc = new QShortcut(QKeySequence::Copy, m_pdfPage);
    copySc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySc, &QShortcut::activated, this,
            [this]() { copyPdfText(CopyScope::Selection); });

    auto *layout = new QVBoxLayout(m_pdfPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_pdfView, 1);
    return m_pdfPage;
}

#if FILECOMMANDER_HAS_PREVIEW_PDF
void QuickView::loadPdfPages() {
    // Tear down any previous document's items first, then build one background item
    // per page of the freshly-loaded m_pdfDoc (its size read now, pixmap + text
    // rendered later, lazily, once the page nears the viewport).
    m_pdfScene->clear(); // deletes all page items + their text children
    m_pdfBgItems.clear();
    m_pdfPageSizes.clear();
    m_pdfPageTop.clear();
    m_pdfRenderedWidth.clear();
    m_pdfTextBuilt.clear();

    if (!m_pdfDoc)
        return;
    const int pageCount = m_pdfDoc->numPages();
    for (int i = 0; i < pageCount; ++i) {
        std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(i));
        const QSizeF sz = page ? page->pageSizeF() : QSizeF(612, 792); // Letter fallback
        m_pdfPageSizes.push_back(sz);

        auto *bg = new QGraphicsPixmapItem;
        bg->setTransformationMode(Qt::SmoothTransformation);
        m_pdfScene->addItem(bg);
        m_pdfBgItems.push_back(bg);
        m_pdfPageTop.push_back(0.0);
        m_pdfRenderedWidth.push_back(-1);
        m_pdfTextBuilt.push_back(false);
    }

    // Lay out page rectangles (sets the scene rect + scrollbar range) then render
    // whatever is initially on screen.
    relayoutPdfPages();
    renderVisiblePdfPages();
}

void QuickView::relayoutPdfPages() {
    if (!m_pdfDoc || m_pdfBgItems.isEmpty())
        return;

    // Fit each page to the viewport width (minus the scrollbar extent and a small
    // margin so no horizontal scrollbar appears), scaled by the user zoom. Guard a
    // small minimum for the case where the pane isn't laid out yet (width ~0); the
    // first resize event then re-fits to the real width.
    const int sbExtent = m_pdfView->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int viewportW = m_pdfView->viewport()->width() - sbExtent - kPdfSideMargin;
    const double baseW = qMax(120, viewportW);
    const double targetW = baseW * m_pdfZoom;

    // Preserve the scroll position as a fraction of the total range so a zoom or
    // resize keeps roughly the same part of the document in view.
    QScrollBar *vbar = m_pdfView->verticalScrollBar();
    const double ratio = vbar->maximum() > 0
                             ? double(vbar->value()) / double(vbar->maximum())
                             : 0.0;

    // Stack pages top to bottom, centred; a fitted page is targetW wide.
    double y = kPdfPageGap;
    for (int i = 0; i < m_pdfBgItems.size(); ++i) {
        const double Wp = qMax(1.0, m_pdfPageSizes[i].width());
        const double Hp = qMax(1.0, m_pdfPageSizes[i].height());
        const double pageW = targetW;
        const double pageH = targetW * Hp / Wp;
        const double x = qMax(0.0, (targetW - pageW) / 2.0); // pages share one width
        m_pdfPageTop[i] = y;
        QGraphicsPixmapItem *bg = m_pdfBgItems[i];
        bg->setPos(x, y);
        // Discard any pixmap + text at the old scale; both are rebuilt lazily.
        bg->setPixmap(QPixmap());
        for (QGraphicsItem *child : bg->childItems())
            delete child;
        m_pdfRenderedWidth[i] = -1;
        m_pdfTextBuilt[i] = false;
        y += pageH + kPdfPageGap;
    }

    m_pdfScene->setSceneRect(0, 0, targetW, y);
    if (ratio > 0.0 && vbar->maximum() > 0)
        vbar->setValue(qRound(ratio * vbar->maximum()));
}

void QuickView::buildPdfPageText(int i) {
    // A transparent, selectable QGraphicsTextItem per word, positioned under the
    // matching bitmap glyphs so a drag over the page selects real text. Word boxes
    // come back in points (72 dpi); scale them to the fitted bitmap width.
    if (i < 0 || i >= m_pdfBgItems.size() || m_pdfTextBuilt[i])
        return;
    std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(i));
    if (!page)
        return;
    QGraphicsPixmapItem *bg = m_pdfBgItems[i];
    const double Wp = qMax(1.0, m_pdfPageSizes[i].width());
    const double scale = double(m_pdfRenderedWidth[i] > 0 ? m_pdfRenderedWidth[i] : Wp) / Wp;

    const QList<Poppler::TextBox *> words = page->textList();
    for (Poppler::TextBox *box : words) {
        const QRectF b = box->boundingBox();
        const QString word = box->text();
        if (word.isEmpty()) {
            continue;
        }
        auto *item = new QGraphicsTextItem(bg);
        item->document()->setDocumentMargin(0);
        QFont f = item->font();
        const int px = qMax(1, qRound(b.height() * scale * 0.85));
        f.setPixelSize(px);
        item->setFont(f);
        item->setDefaultTextColor(QColor(0, 0, 0, 0)); // invisible glyphs
        item->setPlainText(word);
        item->setPos(b.x() * scale, b.y() * scale);
        item->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    qDeleteAll(words); // textList transfers ownership of the boxes
    m_pdfTextBuilt[i] = true;
}

void QuickView::renderVisiblePdfPages() {
    if (!m_pdfDoc || m_pdfBgItems.isEmpty())
        return;

    // Visible window in scene coordinates, padded by one viewport height above and
    // below so scrolling reveals already-rendered pages instead of blanks.
    const QRectF vis = m_pdfView->mapToScene(m_pdfView->viewport()->rect()).boundingRect();
    const double vh = qMax(1.0, vis.height());
    const double keepTop = vis.top() - vh;
    const double keepBottom = vis.bottom() + vh;

    int firstVisible = -1;
    for (int i = 0; i < m_pdfBgItems.size(); ++i) {
        const double Wp = qMax(1.0, m_pdfPageSizes[i].width());
        const double Hp = qMax(1.0, m_pdfPageSizes[i].height());
        const double top = m_pdfPageTop[i];
        const double pageH = (m_pdfRenderedWidth[i] > 0 ? m_pdfRenderedWidth[i]
                                                        : qMax(1.0, m_pdfScene->sceneRect().width())) *
                             Hp / Wp;
        const double bottom = top + pageH;
        const bool inWindow = bottom >= keepTop && top <= keepBottom;
        if (firstVisible < 0 && bottom >= vis.top() && top <= vis.bottom())
            firstVisible = i;

        QGraphicsPixmapItem *bg = m_pdfBgItems[i];
        if (inWindow) {
            const int targetW = qRound(m_pdfScene->sceneRect().width());
            if (m_pdfRenderedWidth[i] != targetW) {
                const double dpi = kPdfBaseDpi * (double(targetW) / Wp);
                std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(i));
                const QImage image = page ? page->renderToImage(dpi, dpi) : QImage();
                if (!image.isNull()) {
                    bg->setPixmap(fc::scanlinedPhosphorPixmap(
                        QPixmap::fromImage(image), fc::previewTint()));
                    m_pdfRenderedWidth[i] = targetW;
                }
            }
            if (!m_pdfTextBuilt[i])
                buildPdfPageText(i);
        } else if (m_pdfRenderedWidth[i] != -1) {
            // Far offscreen: drop the bitmap + text layer to bound memory. The page
            // rectangle (position/size) stays fixed so the scrollbar range holds.
            bg->setPixmap(QPixmap());
            for (QGraphicsItem *child : bg->childItems())
                delete child;
            m_pdfRenderedWidth[i] = -1;
            m_pdfTextBuilt[i] = false;
        }
    }

    if (firstVisible < 0)
        firstVisible = 0;
    m_pdfPageInfo->setText(
        tr("Page %1 / %2").arg(firstVisible + 1).arg(m_pdfBgItems.size()));
}

int QuickView::currentPdfPage() const {
    if (m_pdfBgItems.isEmpty())
        return -1;
    const QRectF vis = m_pdfView->mapToScene(m_pdfView->viewport()->rect()).boundingRect();
    const double y = vis.top();
    int page = 0;
    for (int i = 0; i < m_pdfPageTop.size(); ++i) {
        if (m_pdfPageTop[i] <= y)
            page = i;
        else
            break;
    }
    return page;
}

void QuickView::copyPdfText(CopyScope scope) {
    QString out;
    if (scope == CopyScope::Selection) {
        // Gather the selected text of every word item across the scene.
        for (QGraphicsPixmapItem *bg : m_pdfBgItems) {
            for (QGraphicsItem *child : bg->childItems()) {
                if (auto *t = qgraphicsitem_cast<QGraphicsTextItem *>(child)) {
                    const QString sel = t->textCursor().selectedText();
                    if (!sel.isEmpty()) {
                        if (!out.isEmpty())
                            out.append(' ');
                        out.append(sel);
                    }
                }
            }
        }
        if (out.isEmpty())
            return; // nothing selected: don't clobber the clipboard
    } else if (scope == CopyScope::CurrentPage) {
        const int i = currentPdfPage();
        std::unique_ptr<Poppler::Page> page(i >= 0 ? m_pdfDoc->page(i) : nullptr);
        if (page)
            out = page->text(QRectF()); // whole-page text
    } else { // All
        for (int i = 0; i < m_pdfBgItems.size(); ++i) {
            std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(i));
            if (page) {
                if (!out.isEmpty())
                    out.append(QStringLiteral("\n\n"));
                out.append(page->text(QRectF()));
            }
        }
    }
    if (!out.isEmpty())
        QApplication::clipboard()->setText(out);
}

void QuickView::closePdf() {
    // Releasing the unique_ptr frees the Poppler document; clear all page items and
    // reset the view state so a later PDF starts clean and no stale page lingers.
    m_pdfDoc.reset();
    if (m_pdfScene)
        m_pdfScene->clear();
    m_pdfBgItems.clear();
    m_pdfPageSizes.clear();
    m_pdfPageTop.clear();
    m_pdfRenderedWidth.clear();
    m_pdfTextBuilt.clear();
    m_pdfZoom = 1.0;
    if (m_pdfPageInfo)
        m_pdfPageInfo->clear();
}
#else
void QuickView::loadPdfPages() {}
void QuickView::relayoutPdfPages() {}
void QuickView::buildPdfPageText(int) {}
void QuickView::renderVisiblePdfPages() {}
int QuickView::currentPdfPage() const { return -1; }
void QuickView::copyPdfText(CopyScope) {}
void QuickView::closePdf() {
    if (m_pdfScene)
        m_pdfScene->clear();
    m_pdfBgItems.clear();
    m_pdfPageSizes.clear();
    m_pdfPageTop.clear();
    m_pdfRenderedWidth.clear();
    m_pdfTextBuilt.clear();
}
#endif

QWidget *QuickView::buildSlidesPage() {
    m_slidesPage = new QWidget(this);
    m_slidesPage->setObjectName(QStringLiteral("quickViewSlidesPage"));

    // A slim toolbar: zoom (fit-to-width is the implicit default), copy-text
    // fallbacks, and the "Slide N / M" readout driven by scroll position.
    auto *toolbar = new QToolBar(m_slidesPage);
    toolbar->addAction(tr("Zoom In"), this, [this]() {
        if (m_slidePageTop.isEmpty())
            return;
        m_slidesZoom = qBound(kPdfMinZoom, m_slidesZoom * kZoomStep, kPdfMaxZoom);
        relayoutSlides();
    });
    toolbar->addAction(tr("Zoom Out"), this, [this]() {
        if (m_slidePageTop.isEmpty())
            return;
        m_slidesZoom = qBound(kPdfMinZoom, m_slidesZoom / kZoomStep, kPdfMaxZoom);
        relayoutSlides();
    });
    toolbar->addSeparator();
    toolbar->addAction(tr("Copy Slide"), this,
                       [this]() { copySlidesText(CopyScope::CurrentPage); });
    toolbar->addAction(tr("Copy All"), this,
                       [this]() { copySlidesText(CopyScope::All); });

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    m_slidesInfo = new QLabel(m_slidesPage);
    m_slidesInfo->setContentsMargins(8, 0, 8, 0);
    toolbar->addWidget(m_slidesInfo);

    // Every slide's shapes and text are native graphics items stacked in one scene.
    // The view scales the whole scene to fit the pane width (vectors stay crisp at
    // any zoom); it does not rasterize.
    m_slidesScene = new QGraphicsScene(this);
    m_slidesScene->setBackgroundBrush(palette().color(QPalette::Dark));
    m_slidesView = new QGraphicsView(m_slidesScene, m_slidesPage);
    m_slidesView->setFrameShape(QFrame::NoFrame);
    m_slidesView->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_slidesView->setDragMode(QGraphicsView::NoDrag); // let text items own the mouse
    m_slidesView->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                                 QPainter::SmoothPixmapTransform);
    m_slidesView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Resizing the pane re-fits the deck to the new width; debounce so a divider
    // drag re-fits once at the end rather than on every intermediate width.
    m_slidesView->viewport()->installEventFilter(this);

    m_slidesRelayoutTimer = new QTimer(this);
    m_slidesRelayoutTimer->setSingleShot(true);
    m_slidesRelayoutTimer->setInterval(80);
    connect(m_slidesRelayoutTimer, &QTimer::timeout, this, [this]() {
        if (m_slidePageTop.isEmpty())
            return;
        relayoutSlides();
        // The deferred re-fit is the last step of loading a deck; from here on the
        // user is browsing, so resume preserving the scroll ratio on zoom/resize.
        m_slidesResetScroll = false;
    });

    // Scrolling just updates the slide readout (all items are already in the scene).
    connect(m_slidesView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this]() { renderVisibleSlides(); });

    auto *copySc = new QShortcut(QKeySequence::Copy, m_slidesPage);
    copySc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySc, &QShortcut::activated, this,
            [this]() { copySlidesText(CopyScope::Selection); });

    auto *layout = new QVBoxLayout(m_slidesPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_slidesView, 1);
    return m_slidesPage;
}

namespace {
// A slide's off-screen stand-in: a plain white rect at the slide's size, so the
// scrollbar range and page geometry match a fully built slide without the cost of
// parsing shapes/text or decoding embedded images.
QGraphicsRectItem *makeSlidePlaceholder(const QSizeF &sizeScene) {
    auto *ph = new QGraphicsRectItem(QRectF(QPointF(0, 0), sizeScene));
    ph->setBrush(Qt::white);
    ph->setPen(QPen(QColor(0xcc, 0xcc, 0xcc)));
    return ph;
}
} // namespace

void QuickView::loadSlides(const QStringList &svgs) {
    // Parse every slide's size + text up front (cheap) and lay out one placeholder
    // per slide; the full item tree (shapes/text/images) is built lazily, only for
    // the slides near the viewport, so even a big multi-image deck opens instantly.
    m_slidesScene->clear();
    m_slideSvgs.clear();
    m_slidePageItems.clear();
    m_slideBuilt.clear();
    m_slidePageTop.clear();
    m_slideSizes.clear();
    m_slideTexts.clear();
    m_slidesZoom = 1.0;
    m_slidesSceneWidth = 0.0;
    m_slidesResetScroll = true; // a new deck opens at the top of slide 1

    double y = 0.0;
    for (const QString &svg : svgs) {
        const QByteArray bytes = svg.toUtf8();
        QSizeF sizeScene(960 * SlideScene::kSceneScale, 540 * SlideScene::kSceneScale);
        QString text;
        // Metadata only: no items, no image decode. Copy All/Copy Slide rely on
        // m_slideTexts, so text is extracted for EVERY slide regardless of lazy
        // build.
        SlideScene::parseSlideMeta(bytes, &sizeScene, &text);

        QGraphicsRectItem *ph = makeSlidePlaceholder(sizeScene);
        m_slidesScene->addItem(ph);
        ph->setPos(0, y);

        m_slideSvgs.push_back(bytes);
        m_slidePageItems.push_back(ph);
        m_slideBuilt.push_back(false);
        m_slidePageTop.push_back(y);
        m_slideSizes.push_back(sizeScene);
        m_slideTexts.push_back(text);
        m_slidesSceneWidth = qMax(m_slidesSceneWidth, sizeScene.width());
        const double gap = qMax(200.0, sizeScene.height() * 0.03);
        y += sizeScene.height() + gap;
    }
    m_slidesScene->setSceneRect(0, 0, qMax(1.0, m_slidesSceneWidth), qMax(1.0, y));

    relayoutSlides(); // fits, resets to top, and builds the initially-visible slides
    // When the same QuickView is reused for a different deck, the view's viewport
    // width may not be settled yet, so the fit computed above can be wrong. Re-fit
    // once the layout has run (mirrors the old lazy-render deferral, kept to avoid
    // reintroducing the "blank after switching decks" bug).
    m_slidesRelayoutTimer->start();
}

void QuickView::appendRemainingSlides(const QStringList &fullSvgs) {
    // Stage 2 of a pptx load: the full deck arrived; append the slides past the
    // ones already shown. The first slides are identical to stage 1's, so we keep
    // them (and the user's scroll position / any built items) untouched and only
    // extend the scene downward.
    const int startIdx = m_slideSvgs.size();
    if (fullSvgs.size() <= startIdx)
        return;

    const double S = SlideScene::kSceneScale;
    auto gapAfter = [](double h) { return qMax(200.0, h * 0.03); };

    // Continue stacking from just below the last already-loaded slide.
    double y = 0.0;
    if (!m_slidePageTop.isEmpty())
        y = m_slidePageTop.last() + m_slideSizes.last().height() +
            gapAfter(m_slideSizes.last().height());

    for (int i = startIdx; i < fullSvgs.size(); ++i) {
        const QByteArray bytes = fullSvgs[i].toUtf8();
        QSizeF sizeScene(960 * S, 540 * S);
        QString text;
        SlideScene::parseSlideMeta(bytes, &sizeScene, &text);

        QGraphicsRectItem *ph = makeSlidePlaceholder(sizeScene);
        m_slidesScene->addItem(ph);
        ph->setPos(0, y);

        m_slideSvgs.push_back(bytes);
        m_slidePageItems.push_back(ph);
        m_slideBuilt.push_back(false);
        m_slidePageTop.push_back(y);
        m_slideSizes.push_back(sizeScene);
        m_slideTexts.push_back(text); // Copy All now covers the whole deck
        m_slidesSceneWidth = qMax(m_slidesSceneWidth, sizeScene.width());
        y += sizeScene.height() + gapAfter(sizeScene.height());
    }

    // Growing the scene rect extends the scrollbar range; the current value (and so
    // the visible region, whose slides are unchanged) stays put.
    m_slidesScene->setSceneRect(0, 0, qMax(1.0, m_slidesSceneWidth), qMax(1.0, y));
    renderVisibleSlides(); // build any now-visible appended slides + refresh N / M
}

void QuickView::buildSlideItem(int i) {
    // Swap slide i's placeholder for its full item tree (parsed on demand). Marked
    // built even if the SVG is unparseable, so a broken slide isn't re-parsed on
    // every scroll -- its placeholder simply stays.
    if (i < 0 || i >= m_slidePageItems.size() || m_slideBuilt[i])
        return;
    QGraphicsItem *page = SlideScene::buildSlidePage(m_slideSvgs[i], nullptr, nullptr);
    m_slideBuilt[i] = true;
    if (!page)
        return; // keep the placeholder
    // A slide is a tree of vector items and embedded bitmaps, so unlike the
    // image and PDF paths there is nothing to recolour on the way in -- the
    // effect tints the rasterised page instead. Attached per page (not to the
    // view) so the cost is bounded by one slide, and only when there is a tint.
    if (fc::previewTint().isValid())
        page->setGraphicsEffect(new ttc::PhosphorEffect(fc::previewTint()));
    delete m_slidePageItems[i]; // removes the placeholder from the scene
    m_slidesScene->addItem(page);
    page->setPos(0, m_slidePageTop[i]);
    m_slidePageItems[i] = page;
}

void QuickView::releaseSlideItem(int i) {
    // Swap slide i's full item tree back for a lightweight placeholder, freeing any
    // decoded images once the slide is well off-screen.
    if (i < 0 || i >= m_slidePageItems.size() || !m_slideBuilt[i])
        return;
    delete m_slidePageItems[i];
    QGraphicsRectItem *ph = makeSlidePlaceholder(m_slideSizes[i]);
    m_slidesScene->addItem(ph);
    ph->setPos(0, m_slidePageTop[i]);
    m_slidePageItems[i] = ph;
    m_slideBuilt[i] = false;
}

void QuickView::relayoutSlides() {
    if (m_slidePageTop.isEmpty() || m_slidesSceneWidth <= 0.0)
        return;

    // Fit the scene width to the viewport (minus the scrollbar extent and a small
    // margin so no horizontal scrollbar appears), scaled by the user zoom, via the
    // view transform. Guard a minimum for the not-yet-laid-out case (width ~0); the
    // first resize event then re-fits to the real width.
    const int sbExtent = m_slidesView->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int viewportW = m_slidesView->viewport()->width() - sbExtent - kPdfSideMargin;
    const double baseW = qMax(120, viewportW);
    const double fit = baseW / m_slidesSceneWidth;

    // Preserve the scroll position as a fraction of the range across the transform
    // -- but only while browsing one deck. On a fresh load, force the top so a
    // file switch never inherits the previous deck's scroll ratio (see
    // m_slidesResetScroll).
    QScrollBar *vbar = m_slidesView->verticalScrollBar();
    const double ratio = (!m_slidesResetScroll && vbar->maximum() > 0)
                             ? double(vbar->value()) / double(vbar->maximum())
                             : 0.0;

    const double s = fit * m_slidesZoom;
    m_slidesView->setTransform(QTransform::fromScale(s, s));

    if (m_slidesResetScroll)
        vbar->setValue(0);
    else if (ratio > 0.0 && vbar->maximum() > 0)
        vbar->setValue(qRound(ratio * vbar->maximum()));
    renderVisibleSlides(); // refresh the "Slide N / M" readout for the new geometry
}

void QuickView::renderVisibleSlides() {
    if (m_slidePageTop.isEmpty()) {
        if (m_slidesInfo)
            m_slidesInfo->clear();
        return;
    }

    // Visible band in scene coordinates, padded by one viewport height above and
    // below so a slide is fully built before it scrolls into view (no blank frame).
    const QRectF vis =
        m_slidesView->mapToScene(m_slidesView->viewport()->rect()).boundingRect();
    const double vh = qMax(1.0, vis.height());
    const double keepTop = vis.top() - vh;
    const double keepBottom = vis.bottom() + vh;

    for (int i = 0; i < m_slidePageItems.size(); ++i) {
        const double top = m_slidePageTop[i];
        const double bottom = top + m_slideSizes[i].height();
        const bool inWindow = bottom >= keepTop && top <= keepBottom;
        if (inWindow && !m_slideBuilt[i])
            buildSlideItem(i);
        else if (!inWindow && m_slideBuilt[i])
            releaseSlideItem(i);
    }

    const int cur = qMax(0, currentSlide());
    m_slidesInfo->setText(tr("Slide %1 / %2").arg(cur + 1).arg(m_slidePageTop.size()));
}

int QuickView::currentSlide() const {
    if (m_slidePageTop.isEmpty())
        return -1;
    const QRectF vis =
        m_slidesView->mapToScene(m_slidesView->viewport()->rect()).boundingRect();
    const double y = vis.top();
    int slide = 0;
    for (int i = 0; i < m_slidePageTop.size(); ++i) {
        if (m_slidePageTop[i] <= y + 1.0)
            slide = i;
        else
            break;
    }
    return slide;
}

void QuickView::copySlidesText(CopyScope scope) {
    QString out;
    if (scope == CopyScope::Selection) {
        // Gather the selected text of every selectable text item in the scene.
        const QList<QGraphicsItem *> items = m_slidesScene->items();
        // items() returns top-to-bottom; reverse for roughly reading order.
        for (int i = items.size() - 1; i >= 0; --i) {
            if (auto *t = qgraphicsitem_cast<QGraphicsTextItem *>(items[i])) {
                const QString sel = t->textCursor().selectedText();
                if (!sel.isEmpty()) {
                    if (!out.isEmpty())
                        out.append('\n');
                    out.append(sel);
                }
            }
        }
        if (out.isEmpty())
            return; // nothing selected: don't clobber the clipboard
    } else if (scope == CopyScope::CurrentPage) {
        const int i = currentSlide();
        if (i >= 0 && i < m_slideTexts.size())
            out = m_slideTexts[i];
    } else { // All
        for (const QString &t : m_slideTexts) {
            if (!out.isEmpty())
                out.append(QStringLiteral("\n\n"));
            out.append(t);
        }
    }
    if (!out.isEmpty())
        QApplication::clipboard()->setText(out);
}

void QuickView::closeSlides() {
    // Drop all slide items and data, and reset the view state so a later deck
    // starts clean and no stale slide lingers.
    if (m_slidesScene)
        m_slidesScene->clear();
    m_slideSvgs.clear();
    m_slidePageItems.clear();
    m_slideBuilt.clear();
    m_slidePageTop.clear();
    m_slideSizes.clear();
    m_slideTexts.clear();
    m_slidesSceneWidth = 0.0;
    m_slidesZoom = 1.0;
    m_slidesResetScroll = false;
    if (m_slidesInfo)
        m_slidesInfo->clear();
}

void QuickView::zoomImageBy(double factor, bool debounce) {
    m_imageFitMode = false;
    m_imageScale = qBound(kMinScale, m_imageScale * factor, kMaxScale);
    if (debounce)
        m_imageWheelRenderTimer->start();
    else
        requestImageRender();
}

bool QuickView::eventFilter(QObject *watched, QEvent *event) {
    // Tab/Backtab while typing a preview password returns focus to whatever held
    // it before the encrypted page appeared (the file list), instead of cycling
    // to the Unlock button.
    if (watched == m_passwordEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            if (m_focusBeforeEncrypted && m_focusBeforeEncrypted->isVisible()) {
                m_focusBeforeEncrypted->setFocus();
                return true;
            }
        }
    }
    if (watched == m_videoSurface && event->type() == QEvent::Resize) {
        positionVideoInfoOverlay(); // keep the panel pinned to the top-right corner
        // Reapply the preview filter after the output surface changes. This is
        // idempotent: applyVideoFilter skips an unchanged filter string.
        applyVideoPhosphor();
        // fall through to default handling
    }
    // Resizing the PDF viewport re-fits every page to the new width; debounce so a
    // divider drag re-fits once at the end rather than on every intermediate width.
    if (m_pdfView && watched == m_pdfView->viewport() &&
        event->type() == QEvent::Resize) {
        if (m_pdfDoc)
            m_pdfRelayoutTimer->start();
        // fall through to default handling
    }
    // Same debounced re-fit for the slides viewport.
    if (m_slidesView && watched == m_slidesView->viewport() &&
        event->type() == QEvent::Resize) {
        if (!m_slidePageTop.isEmpty())
            m_slidesRelayoutTimer->start();
        // fall through to default handling
    }
    const bool onImage =
        watched == m_imageLabel || watched == m_imageScroll->viewport();
    if (watched == m_imageScroll->viewport() && event->type() == QEvent::Resize) {
        positionInfoOverlay(); // keep the panel pinned to the top-right corner
        // ...and re-fit, which is the only chance the FIRST image gets. A
        // QStackedLayout lays out just its current page, so until this page is
        // revealed the viewport is still Qt's default ~100x30 -- and the fit
        // was computed against that, when the image finished loading. Every
        // later image is fitted correctly because the page is current by then,
        // which is why only the first one looked wrong. The PDF and slide
        // viewports just above already refit on this event; the image path was
        // the odd one out.
        if (m_imageFitMode && !m_originalImage.isNull())
            m_refitTimer->start();
        // fall through to default handling
    }
    if (onImage) {
        switch (event->type()) {
        case QEvent::Wheel: {
            auto *we = static_cast<QWheelEvent *>(event);
            zoomImageBy(we->angleDelta().y() > 0 ? kZoomStep : 1.0 / kZoomStep, true);
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
                const QPixmap *pixmap = m_imageLabel->pixmap();
                updateImageCursor(pixmap ? pixmap->size() : QSize());
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
    if (m_imageFitMode && !m_originalImage.isNull() && m_stack->currentWidget() == m_imagePage)
        m_refitTimer->start();
}

bool QuickView::canStreamPreview(const QString &path) {
    // Video only. Audio would work at the mpv level too, but the audio page is
    // built from bytes read off the local file:
    // Id3Reader::read() opens it with QFile for tags and cover art, and the
    // prev/next track buttons come from scanning the containing directory.
    // Streaming would silently empty all of that, and audio files are small
    // enough that downloading them was never the complaint.
#if FILECOMMANDER_MEDIA_BACKEND_MPV
    return isVideo(path);
#else
    Q_UNUSED(path);
    return false;
#endif
}

void QuickView::showFile(const QString &path) {
    const bool reloadWaitsForRotation =
        path == m_imagePath && m_stack->currentWidget() == m_imagePage &&
        m_pendingImageRotations.value(path) > 0 && m_imageLabel->pixmap();
    cancelPendingPreviewWork();
    m_originalImage = {};
    m_imageTransform.reset();
    m_imagePath.clear();

    QFileInfo info(path);
    // A stream URL names a remote file being read through its FileProvider, so
    // none of the local-filesystem questions below apply to it -- there is
    // deliberately no file on disk to stat.
#if FILECOMMANDER_MEDIA_BACKEND_MPV
    const bool streamed = MpvStreamSource::isStreamUrl(path);
#else
    const bool streamed = false;
#endif
    // An explicit encoding override belongs to the currently displayed text file
    // only. Re-selecting that same text page preserves it; every different file
    // begins in Auto. Hex is intentionally separate and therefore persists.
    const bool preserveTextEncoding =
        path == m_textPath && m_stack->currentWidget() == m_textPage;
    m_textPath.clear();
    if (path.isEmpty() || (!streamed && (!info.exists() || info.isDir()))) {
        stopVideo();
        stopAudio();
        m_infoOverlay->hide();
        m_info->setText(tr("Select a file to preview"));
        revealStaticPage(m_info);
        return;
    }

    // Archives: a read-only listing of their contents (a pure header scan, no
    // extraction). Checked first so an archive under the cursor shows its file
    // tree instead of falling through to a garbage text head.
    if (ArchiveHandler::isSupportedArchive(path)) {
        ensureArchivePage();
        previewArchive(path);
        return;
    }

    if (isVideo(path)) {
        warmMediaEngine();
        if (!m_mediaEngineReady)
            return;
        ensureVideoPage();
        m_infoOverlay->hide(); // image overlay belongs to another page
        stopAudio();           // don't leave an audio track playing behind the video

        // Already showing/playing this exact clip? Don't reload it -- a spurious
        // re-selection of the same row shouldn't restart the decode.
        if (path == m_videoPath && m_stack->currentWidget() == m_videoPage)
            return;
        m_videoPath = path;
        // A new clip starts the right way up: the rotation was a correction for
        // the last one, and carrying it over would silently misorient this one.
        m_videoRotation = 0;
        m_mediaEngine->setVideoRotation(0);

        // Apply the persisted preview preferences to both the core and controls
        // (block signals so seeding them doesn't re-persist or fight the core).
        const int savedVolume = m_settings.videoVolume();
        const bool savedMuted = m_settings.videoMuted();
        const double savedSpeed = m_settings.videoSpeed();

        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(savedVolume);
        m_volumeSlider->blockSignals(false);
        m_mediaEngine->setVolume(savedVolume);

        m_muteButton->blockSignals(true);
        m_muteButton->setChecked(savedMuted);
        refreshMediaControlIcons();
        m_muteButton->blockSignals(false);
        m_mediaEngine->setMute(savedMuted);

        int speedIndex = m_speedCombo->findData(savedSpeed);
        if (speedIndex < 0)
            speedIndex = 0; // unknown saved speed → fall back to 1x
        m_speedCombo->blockSignals(true);
        m_speedCombo->setCurrentIndex(speedIndex);
        m_speedCombo->blockSignals(false);
        m_mediaEngine->setSpeed(m_speedCombo->itemData(speedIndex).toDouble());

        m_progressSlider->setValue(0);
        m_playButton->setText(tr("Pause")); // loadfile starts playing
        // Pushed before loadfile so the first decoded frame is already tinted;
        // applyVideoFilter is a no-op when the chain is unchanged, which it is
        // for every load between theme switches.
        applyVideoPhosphor();
        m_mediaEngine->load(MediaSource{path, {}, streamed}, MediaKind::Video);
        if (m_mediaEngine->state() == MediaState::Failed)
            return;
        m_stack->setCurrentWidget(m_videoPage);
        releaseHiddenDocumentPages(m_videoPage);
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

    if (isAudio(path)) {
        warmMediaEngine();
        if (!m_mediaEngineReady)
            return;
        ensureAudioPage();
        showAudio(path);
        return;
    }

    // Any non-audio target: stop audio playback if a track was playing.
    stopAudio();

    if (isPdf(path)) {
#if FILECOMMANDER_HAS_PREVIEW_PDF
        m_infoOverlay->hide(); // image overlay belongs to another page
        const int gen = ++m_pdfGen;
        auto *watcher = new QFutureWatcher<std::shared_ptr<Poppler::Document>>(this);
        connect(watcher,
                &QFutureWatcher<std::shared_ptr<Poppler::Document>>::finished, this,
                [this, watcher, gen, path]() {
                    std::shared_ptr<Poppler::Document> doc = watcher->result();
                    watcher->deleteLater();
                    if (gen != m_pdfGen)
                        return;
                    if (!doc || doc->isLocked()) {
                        m_info->setText(
                            tr("Cannot open PDF: %1").arg(QFileInfo(path).fileName()));
                        revealStaticPage(m_info);
                        return;
                    }

                    doc->setRenderHint(Poppler::Document::Antialiasing, true);
                    doc->setRenderHint(Poppler::Document::TextAntialiasing, true);
                    closePdf();
                    m_pdfDoc = std::move(doc);
                    m_pdfZoom = 1.0;
                    ensurePdfPage();
                    // Give the lazy page its real stack geometry before fitting. The
                    // event loop cannot paint this intermediate state.
                    m_stack->setCurrentWidget(m_pdfPage);
                    loadPdfPages();
                    revealStaticPage(m_pdfPage);
                });
        watcher->setFuture(QtConcurrent::run([path]() {
            return std::shared_ptr<Poppler::Document>(Poppler::Document::load(path));
        }));
        return;
#else
        m_info->setText(tr("PDF preview is not enabled in this build: %1").arg(info.fileName()));
        revealStaticPage(m_info);
        return;
#endif
    }

    // Office de-dup: the embedded preview follows the file-list cursor, which can
    // re-fire on the same row. If we're already showing this office file's content,
    // don't reconvert (or tear the current deck down below and rebuild it).
    if (OfficeConverter::isOfficeFile(path) && OfficeConverter::isAvailable() &&
        path == m_officeShownPath &&
        (m_stack->currentWidget() == m_slidesPage ||
         m_stack->currentWidget() == m_markdown ||
         m_stack->currentWidget() == m_officeTabs)) {
        return;
    }

    // Office documents (integrated, always-on): docx/doc/pptx/ppt render as
    // Markdown, xlsx/xls as a grid, via the external office_oxide CLI. Silently
    // skipped only when the CLI isn't installed.
    if (OfficeConverter::isOfficeFile(path) && OfficeConverter::isAvailable()) {
        m_infoOverlay->hide();
        renderOffice(path, QString()); // no password yet; prompts inline if needed
        return;
    }

    if (isMarkdown(path)) {
        m_infoOverlay->hide();
        QFile mdFile(path);
        if (mdFile.open(QIODevice::ReadOnly)) {
            mdFile.close();
            // Parse + lay out Markdown on a worker thread: setMarkdown plus the
            // QTextDocument layout is heavy on dense/table-rich files and would
            // otherwise freeze the GUI. loadMarkdownAsync installs the finished
            // document when it's ready.
            loadMarkdownAsync(path);
            return;
        }
        // Unreadable: fall through to the generic text/no-preview handling below.
    }

    if (ImageViewer::isImage(path)) {
        m_infoOverlay->hide();
        if (reloadWaitsForRotation) {
            preserveImageTransitionSnapshot();
            m_imageLabel->clear();
        }
        m_imageRevealPending = true;
        m_pendingImagePath = path;
        m_pendingImageLoadGeneration = m_imageLoader->load(path);
        m_imageGeneration = qMax(m_imageGeneration, m_pendingImageLoadGeneration);
        return;
    }

    m_infoOverlay->hide(); // no image behind it on the text / no-preview pages
    if (QFileInfo(path).isFile()) {
        // Read and sniff off-thread. It is capped (5 MiB), so it is not the
        // seconds an archive costs -- but it is a synchronous read plus an
        // encoding detection over the whole probe, on the GUI thread, on every
        // cursor move, and on a slow or removable medium that is a visible
        // hitch. Nothing here needs a widget, so none of it belongs on this
        // thread.
        struct TextProbe {
            QByteArray bytes;
            bool complete = false;
            bool opened = false;
        };
        const qint64 cap = m_textCap;
        const quint64 generation = ++m_textLoadGeneration;
        m_textLoadPending = true;
        auto *watcher = new QFutureWatcher<TextProbe>(this);
        connect(watcher, &QFutureWatcher<TextProbe>::finished, this,
                [this, watcher, path, preserveTextEncoding, generation, info]() {
                    const TextProbe probe = watcher->result();
                    watcher->deleteLater();
                    if (generation != m_textLoadGeneration)
                        return; // the cursor moved on while this was reading
                    m_textLoadPending = false;
                    if (!probe.opened) {
                        m_info->setText(tr("No preview available for %1").arg(info.fileName()));
                        revealStaticPage(m_info);
                        return;
                    }
                    m_textAutoResult = TextEncodingDetector::detect(
                        probe.bytes, probe.complete
                                         ? TextEncodingDetector::InputEnd::Complete
                                         : TextEncodingDetector::InputEnd::MayBeTruncated);
                    m_textAutoResultValid = true;
                    m_textTruncated = probe.bytes.size() > m_textCap;
                    m_textRaw = m_textTruncated
                                    ? TextEncodingDetector::safePrefix(
                                          probe.bytes, static_cast<int>(m_textCap),
                                          m_textAutoResult)
                                    : probe.bytes;
                    if (!preserveTextEncoding) {
                        m_textEncoding->blockSignals(true);
                        m_textEncoding->setCurrentIndex(0); // every new file starts in Auto
                        m_textEncoding->blockSignals(false);
                    }
                    m_textPath = path;
                    renderText();
                    revealStaticPage(m_textPage);
                });
        watcher->setFuture(QtConcurrent::run([path, cap]() {
            TextProbe probe;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return probe;
            probe.opened = true;
            probe.bytes = file.read(cap + kTextReadLookAheadBytes);
            probe.complete = file.atEnd();
            return probe;
        }));
        return;
    }

    m_info->setText(tr("No preview available for %1").arg(info.fileName()));
    revealStaticPage(m_info);
}
