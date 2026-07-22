#include "QuickView.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QBuffer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextCodec>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <poppler-qt5.h>

#include "ArchiveHandler.h"
#include "ArchiveModel.h"
#include "ImageViewer.h"
#include "MpvWidget.h"
#include "OfficeConverter.h"
#include "config/Settings.h"

namespace {
constexpr qint64 kTextWindowBytes = 5 * 1024 * 1024; // text preview cap: 5 MiB
constexpr qint64 kMarkdownMaxBytes = 2 * 1024 * 1024; // cap markdown at 2 MiB

// Selectable text encodings for the F3 window's text page. codec == nullptr
// means "use the locale codec".
struct TextEncoding {
    const char *label;
    const char *codec;
};
const TextEncoding kTextEncodings[] = {
    {"UTF-8", "UTF-8"},       {"UTF-16", "UTF-16"},             {"ISO-8859-1", "ISO-8859-1"},
    {"GB18030", "GB18030"},   {"Windows-1252", "Windows-1252"}, {"System", nullptr},
};
constexpr double kZoomStep = 1.25;
constexpr double kMinScale = 0.05;
constexpr double kMaxScale = 20.0;
// PDF rendering: Poppler's renderToImage takes dpi; 72 dpi renders a page at
// its native point size (1.0 zoom). We scale that base by the zoom factor.
constexpr double kPdfBaseDpi = 72.0;
constexpr double kPdfMinZoom = 0.25;
constexpr double kPdfMaxZoom = 6.0;
} // namespace

QuickView::QuickView(Settings &settings, Context context, QWidget *parent)
    : QWidget(parent), m_settings(settings), m_context(context) {
    // Both contexts read up to 5 MiB and show the same toolbar, so the embedded
    // Ctrl+Q pane and the F3 window preview text files identically.
    Q_UNUSED(context);
    m_textCap = kTextWindowBytes;

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
    m_stack->addWidget(m_info);              // 0
    m_stack->addWidget(buildImagePage());    // 1
    m_stack->addWidget(buildTextPage());     // 2
    m_stack->addWidget(buildVideoPage());    // 3
    m_stack->addWidget(buildMarkdownPage());   // 4
    m_stack->addWidget(buildPdfPage());        // 5
    m_stack->addWidget(buildOfficeTablePage());  // 6
    m_stack->addWidget(buildEncryptedPage());    // 7
    m_stack->addWidget(buildArchivePage());      // 8

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
}

// Defined here (not defaulted in the header) so the unique_ptr<Poppler::Document>
// member is destroyed where the complete Poppler type is visible.
QuickView::~QuickView() = default;

void QuickView::setContentFontSize(int pt) {
    if (pt <= 0)
        return;
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
    if (m_officeTable) {
        // Spreadsheet (xls/xlsx) preview grid: scale the cell text with the app
        // font. Cells inherit the widget font, so setting it here is enough; nudge
        // the row height so larger text isn't clipped.
        QFont f = m_officeTable->font();
        f.setPointSize(pt);
        m_officeTable->setFont(f);
        m_officeTable->verticalHeader()->setDefaultSectionSize(QFontMetrics(f).height() + 6);
        if (m_officeTable->rowCount() > 0)
            m_officeTable->resizeColumnsToContents(); // re-fit widths to the new size
    }
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
        target = m_pdfScroll;
    else if (page == m_officeTable)
        target = m_officeTable;
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
    toolbar->addAction(tr("< Prev"), this, [this]() { showPrevSibling(); });
    toolbar->addAction(tr("Next >"), this, [this]() { showNextSibling(); });

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

void QuickView::rotateCurrentImage(int degrees) {
    if (m_originalPixmap.isNull() || m_imagePath.isEmpty())
        return;

    // Rotate what's on screen first for responsiveness. 90-degree multiples are
    // exact, so a fast (nearest-neighbour) transform loses nothing.
    QTransform t;
    t.rotate(degrees);
    m_originalPixmap = m_originalPixmap.transformed(t, Qt::FastTransformation);
    if (m_imageFitMode)
        m_imageScale = fitScale();
    applyImageScale();

    // The overlay's width/height are now swapped; rebuild it if it's showing.
    if (m_infoOverlay->isVisible()) {
        const QFileInfo fi(m_imagePath);
        QImageReader reader(m_imagePath);
        const QString format = QString::fromLatin1(reader.format()).toUpper();
        m_infoOverlay->setText(tr("<b>%1</b><br>%2 &times; %3<br>%4<br>%5 bpp")
                                   .arg(fi.fileName().toHtmlEscaped())
                                   .arg(m_originalPixmap.width())
                                   .arg(m_originalPixmap.height())
                                   .arg(format.isEmpty() ? tr("Unknown format") : format)
                                   .arg(m_originalPixmap.depth()));
        positionInfoOverlay();
    }

    // Persist losslessly back to disk, preserving format and precision.
    const QFileInfo fi(m_imagePath);
    const QString suffix = fi.suffix().toLower();
    const int rot = ((degrees % 360) + 360) % 360; // +90 -> 90, -90 -> 270
    bool saved = false;

    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        // Prefer true-lossless JPEG rotation via jpegtran (no re-encode of the
        // DCT coefficients). Write to a same-directory temp file, then swap.
        const QString jpegtran = QStandardPaths::findExecutable(QStringLiteral("jpegtran"));
        if (!jpegtran.isEmpty()) {
            const QString tmp =
                fi.absoluteDir().filePath(QStringLiteral(".%1.rot.tmp").arg(fi.fileName()));
            QProcess proc;
            proc.start(jpegtran, {QStringLiteral("-rotate"), QString::number(rot),
                                  QStringLiteral("-copy"), QStringLiteral("all"),
                                  QStringLiteral("-outfile"), tmp, m_imagePath});
            if (proc.waitForFinished(15000) && proc.exitStatus() == QProcess::NormalExit &&
                proc.exitCode() == 0 && QFileInfo::exists(tmp)) {
                if (QFile::remove(m_imagePath) && QFile::rename(tmp, m_imagePath))
                    saved = true;
                else
                    QFile::remove(tmp); // leave the original in place on a failed swap
            } else {
                QFile::remove(tmp); // clean up a partial output
            }
        }
        if (!saved) {
            // jpegtran missing or failed: re-encode at max quality (near-lossless).
            QImage img(m_imagePath);
            if (!img.isNull()) {
                img = img.transformed(t, Qt::FastTransformation);
                QImageWriter writer(m_imagePath);
                writer.setQuality(100);
                saved = writer.write(img);
            }
        }
    } else {
        // png/bmp/tiff/webp/...: these round-trip losslessly through QImageWriter.
        QImage img(m_imagePath);
        if (!img.isNull()) {
            img = img.transformed(t, Qt::FastTransformation);
            QImageWriter writer(m_imagePath);
            saved = writer.write(img);
        }
    }

    if (!saved) {
        // Read-only file or unsupported writer: keep the on-screen rotation but
        // make clear the file on disk is unchanged.
        m_infoOverlay->setText(tr("Rotated on screen only — could not save to disk."));
        m_infoOverlay->show();
        positionInfoOverlay();
    }
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
    out.reserve(data.size() * 4);
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
    QString content;
    if (m_textHex) {
        content = toHexDump(m_textRaw);
    } else {
        const char *codecName = kTextEncodings[m_textEncoding->currentIndex()].codec;
        QTextCodec *codec =
            codecName ? QTextCodec::codecForName(codecName) : QTextCodec::codecForLocale();
        if (!codec)
            codec = QTextCodec::codecForName("UTF-8");
        content = codec->toUnicode(m_textRaw);
    }
    if (m_textTruncated)
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
    if (m_stack->currentWidget() != m_imagePage || m_imageSiblings.isEmpty() ||
        m_imageSiblingIndex < 0)
        return;
    showFile(m_imageSiblings.at((m_imageSiblingIndex + 1) % m_imageSiblings.size()));
}

void QuickView::showPrevSibling() {
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
    return kVideoSuffixes.contains(QFileInfo(path).suffix().toLower());
}

bool QuickView::isMarkdown(const QString &path) {
    static const QSet<QString> kMarkdownSuffixes = {"md", "markdown", "mkd", "mdown"};
    return kMarkdownSuffixes.contains(QFileInfo(path).suffix().toLower());
}

bool QuickView::isPdf(const QString &path) {
    return QFileInfo(path).suffix().toLower() == QLatin1String("pdf");
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
        m_mpv->playPause();
        // Reflect the resulting state; playPause is async so query after a beat.
        QTimer::singleShot(50, this, [this]() {
            m_playButton->setText((m_mpv->paused() || m_mpv->ended()) ? tr("Play")
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
                m_mpv->setSpeed(speed);
                m_settings.setVideoSpeed(speed); // persist for later previews
            });

    m_progressSlider = new QSlider(Qt::Horizontal, m_videoPage);
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setToolTip(tr("Seek"));
    connect(m_progressSlider, &QSlider::sliderPressed, this, [this]() { m_seeking = true; });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this]() {
        m_mpv->seekFraction(m_progressSlider->value() / 1000.0);
        m_seeking = false;
    });

    // Mute toggle: checkable, checked == muted. The icon reflects the state so a
    // muted clip reads as muted at a glance.
    m_muteButton = new QPushButton(m_videoPage);
    m_muteButton->setCheckable(true);
    m_muteButton->setToolTip(tr("Mute / unmute"));
    // Bump the icon size so the speaker glyph is proportionate to the button
    // instead of a tiny centred dot. This applies to every icon set on the
    // button (initial, toggle, and showFile), so it only needs setting once.
    m_muteButton->setIconSize(QSize(18, 18));
    auto syncMuteIcon = [this]() {
        m_muteButton->setIcon(style()->standardIcon(
            m_muteButton->isChecked() ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    };
    syncMuteIcon();
    connect(m_muteButton, &QPushButton::toggled, this, [this, syncMuteIcon](bool muted) {
        m_mpv->setMute(muted);
        m_settings.setVideoMuted(muted);
        syncMuteIcon();
    });

    auto *volumeLabel = new QLabel(tr("Vol"), m_videoPage);
    m_volumeSlider = new QSlider(Qt::Horizontal, m_videoPage);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(70);
    m_volumeSlider->setFixedWidth(90);
    m_volumeSlider->setToolTip(tr("Volume"));
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_mpv->setVolume(value);
        m_settings.setVideoVolume(value); // persist for later previews
        // Dragging the volume up is an intent to hear it: lift the mute.
        if (value > 0 && m_muteButton->isChecked())
            m_muteButton->setChecked(false); // its toggle handler unmutes + persists
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
    controls->addWidget(m_muteButton);
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
        // Keep the play/pause label in sync with the core: a clip that's paused
        // or sitting at EOF shows "Play" (clicking replays/resumes).
        m_playButton->setText((m_mpv->paused() || m_mpv->ended()) ? tr("Play")
                                                                       : tr("Pause"));
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

QWidget *QuickView::buildMarkdownPage() {
    // A read-only rich-text browser. Qt renders Markdown through its bundled
    // MD4C parser (QTextDocument::setMarkdown), so no external md4c is linked.
    // Open links in the user's browser rather than trying to navigate in-panel.
    m_markdown = new QTextBrowser(this);
    m_markdown->setOpenExternalLinks(true);
    // office_oxide (and Markdown) tables carry no cell borders; QTextDocument
    // draws none by default. A default stylesheet gives every table cell a thin
    // border so Word/Excel/Markdown tables are legible as grids.
    m_markdown->document()->setDefaultStyleSheet(
        QStringLiteral("table { border-collapse: collapse; } "
                       "td, th { border: 1px solid #808080; padding: 2px 6px; }"));
    return m_markdown;
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
    m_officeTable = new QTableWidget(this);
    m_officeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_officeTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_officeTable->horizontalHeader()->setVisible(false);
    m_officeTable->verticalHeader()->setVisible(false);
    return m_officeTable;
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
    m_encryptedFeedback->setStyleSheet(QStringLiteral("color: #d33;"));
    column->addWidget(m_encryptedFeedback);

    outer->addLayout(column);
    outer->addStretch(1);

    // Enter in the field or the button both attempt to unlock.
    connect(m_unlockButton, &QPushButton::clicked, this, &QuickView::tryUnlock);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &QuickView::tryUnlock);
    return m_encryptedPage;
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
        renderOffice(m_encryptedPath, password);
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
    const QFileInfo info(path);
    const OfficeConverter::Result r = OfficeConverter::convert(path, password);

    if (r.ok && r.kind == OfficeConverter::Kind::Document) {
        // Fit large embedded images to the preview width. Use the stack's width
        // (the actual pane width) rather than m_markdown's, which may still be
        // stale until it's shown as the current page below.
        const int avail = qMax(200, m_stack->width() - 32);
        m_markdown->setHtml(fitImagesToWidth(r.html, avail));
        m_stack->setCurrentWidget(m_markdown);
        return;
    }
    if (r.ok && r.kind == OfficeConverter::Kind::Spreadsheet) {
        populateCsvTable(r.tsv);
        m_stack->setCurrentWidget(m_officeTable);
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
        return;
    case OfficeConverter::Encryption::WrongPassword:
        // Stay on the page and report in place; let the user retype.
        m_encryptedKind = EncryptedKind::Office;
        m_encryptedPath = path;
        m_encryptedFeedback->setText(tr("Incorrect password. Try again."));
        m_passwordEdit->selectAll();
        m_passwordEdit->setFocus();
        m_stack->setCurrentWidget(m_encryptedPage);
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
        return;
    case OfficeConverter::Encryption::None:
        break;
    }

    m_info->setText(tr("Cannot preview %1:\n%2").arg(info.fileName(), r.error));
    m_stack->setCurrentWidget(m_info);
}

QWidget *QuickView::buildArchivePage() {
    m_archivePage = new QWidget(this);
    m_archiveModel = new ArchiveModel(this);

    auto *toolbar = new QToolBar(m_archivePage);
    toolbar->addAction(tr("Up"), this, &QuickView::navigateArchiveUp);
    m_archivePathLabel = new QLabel(m_archivePage);
    m_archivePathLabel->setContentsMargins(8, 0, 8, 0);
    toolbar->addWidget(m_archivePathLabel);

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
    layout->addWidget(m_archiveView, 1);
    return m_archivePage;
}

void QuickView::previewArchive(const QString &path) {
    stopVideo();
    closePdf();
    m_infoOverlay->hide();
    // Fresh chain rooted at this archive; drop any previously extracted nesteds.
    m_archivePaths = QStringList{path};
    m_archivePasswords = QStringList{QString()};
    m_nestedDir.reset();
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
        m_stack->setCurrentWidget(m_archivePage);
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

    m_info->setText(tr("Loading %1…").arg(fi.fileName()));
    m_stack->setCurrentWidget(m_info);

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
        return r;
    }));
}

void QuickView::handleArchiveLoad(const ArchiveLoadResult &r, const QString &path,
                                  const QString &pw, qint64 size, qint64 mtime) {
    const QString name = QFileInfo(path).fileName();

    if (r.status == ArchiveHandler::Status::Ok && r.root) {
        // Cache the tree (bounded, FIFO eviction) so re-visits and "Up" are instant.
        m_archiveCache.insert(path, {r.root, pw, size, mtime});
        m_archiveCacheOrder.removeAll(path);
        m_archiveCacheOrder.append(path);
        constexpr int kMaxCachedArchives = 8;
        while (m_archiveCacheOrder.size() > kMaxCachedArchives)
            m_archiveCache.remove(m_archiveCacheOrder.takeFirst());

        m_archiveModel->setTree(r.root, path, pw);
        updateArchivePathLabel();
        m_stack->setCurrentWidget(m_archivePage);
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
        break;
    }
    case ArchiveHandler::Status::WrongPassword:
        m_encryptedKind = EncryptedKind::Archive;
        m_encryptedPath = path;
        m_encryptedFeedback->setText(tr("Incorrect password. Try again."));
        m_passwordEdit->selectAll();
        m_passwordEdit->setFocus();
        m_stack->setCurrentWidget(m_encryptedPage);
        break;
    case ArchiveHandler::Status::EncryptedUnsupported:
        m_info->setText(
            tr("“%1” uses an encryption that can't be previewed.").arg(name));
        m_stack->setCurrentWidget(m_info);
        break;
    default:
        m_info->setText(tr("Cannot open archive: %1").arg(name));
        m_stack->setCurrentWidget(m_info);
        break;
    }
}

void QuickView::descendIntoNestedArchive(const QString &entryFullPath, const QString &entryName) {
    if (!m_nestedDir)
        m_nestedDir = std::make_unique<QTemporaryDir>();
    if (!m_nestedDir->isValid()) {
        m_info->setText(tr("Could not create a temporary directory."));
        m_stack->setCurrentWidget(m_info);
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
        m_stack->setCurrentWidget(m_info);
        return;
    }
    const QString nested = QDir(sub).filePath(entryFullPath);
    if (!QFileInfo::exists(nested)) {
        m_info->setText(tr("Could not read the nested archive %1.").arg(entryName));
        m_stack->setCurrentWidget(m_info);
        return;
    }
    m_archivePaths.append(nested);
    m_archivePasswords.append(QString());
    tryLoadCurrentArchive();
}

void QuickView::onArchiveActivated(const QModelIndex &index) {
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

void QuickView::populateCsvTable(const QString &tsv) {
    m_officeTable->clear();
    // office-oxide's `text` output is tab-separated: one row per line, cells
    // split on '\t' (commas are literal, so no quote handling needed).
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
    m_officeTable->setRowCount(rows.size());
    m_officeTable->setColumnCount(cols);
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows.at(r).size(); ++c)
            m_officeTable->setItem(r, c, new QTableWidgetItem(rows.at(r).at(c)));
    m_officeTable->resizeColumnsToContents();
}

namespace {
// Horizontal breathing room subtracted from the viewport before fitting pages to
// width, on top of the scrollbar extent, so a page never overflows into a
// horizontal scrollbar. Also the vertical margin (in viewport heights) of pages
// kept rendered above/below the visible window.
constexpr int kPdfSideMargin = 12;
} // namespace

QWidget *QuickView::buildPdfPage() {
    m_pdfPage = new QWidget(this);

    // A slim toolbar: just zoom (fit-to-width is the implicit default), with the
    // "page N / M" readout — driven by scroll position, not a current-page index.
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

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    m_pdfPageInfo = new QLabel(m_pdfPage);
    m_pdfPageInfo->setContentsMargins(8, 0, 8, 0);
    toolbar->addWidget(m_pdfPageInfo);

    // One container widget holds a top-aligned column of per-page labels; the
    // scroll area does not resize it (we size labels ourselves to control fit).
    m_pdfContainer = new QWidget;
    auto *pagesLayout = new QVBoxLayout(m_pdfContainer);
    pagesLayout->setContentsMargins(4, 4, 4, 4);
    pagesLayout->setSpacing(8);
    pagesLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    m_pdfScroll = new QScrollArea(m_pdfPage);
    m_pdfScroll->setWidget(m_pdfContainer);
    m_pdfScroll->setWidgetResizable(false);
    m_pdfScroll->setAlignment(Qt::AlignHCenter);
    // Resizing the pane re-fits every page to the new width; debounce so a divider
    // drag re-fits once at the end rather than on every intermediate width.
    m_pdfScroll->viewport()->installEventFilter(this);

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
    connect(m_pdfScroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this]() { renderVisiblePdfPages(); });

    auto *layout = new QVBoxLayout(m_pdfPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_pdfScroll, 1);
    return m_pdfPage;
}

void QuickView::loadPdfPages() {
    // Tear down any previous document's labels first, then build one placeholder
    // label per page of the freshly-loaded m_pdfDoc.
    if (auto *lay = m_pdfContainer->layout()) {
        while (QLayoutItem *item = lay->takeAt(0)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
    }
    m_pdfPageLabels.clear();
    m_pdfPageSizes.clear();
    m_pdfRenderedWidth.clear();

    if (!m_pdfDoc)
        return;
    const int pageCount = m_pdfDoc->numPages();
    auto *lay = static_cast<QVBoxLayout *>(m_pdfContainer->layout());
    for (int i = 0; i < pageCount; ++i) {
        // Open each page once just to read its native size; the pixmap is rendered
        // later, lazily, only when the page nears the viewport.
        std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(i));
        const QSizeF sz = page ? page->pageSizeF() : QSizeF(612, 792); // Letter fallback
        m_pdfPageSizes.push_back(sz);

        auto *label = new QLabel(m_pdfContainer);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("QLabel { background: white; }"));
        lay->addWidget(label, 0, Qt::AlignHCenter);
        m_pdfPageLabels.push_back(label);
        m_pdfRenderedWidth.push_back(-1);
    }

    // Size every placeholder to its fitted dimensions so the scrollbar range is
    // correct up front, then render whatever is initially on screen.
    relayoutPdfPages();
    renderVisiblePdfPages();
}

void QuickView::relayoutPdfPages() {
    if (!m_pdfDoc || m_pdfPageLabels.isEmpty())
        return;

    // Fit each page to the viewport width (minus the scrollbar extent and a small
    // margin so no horizontal scrollbar appears), scaled by the user zoom. Guard a
    // small minimum for the case where the pane isn't laid out yet (width ~0); the
    // first resize event then re-fits to the real width.
    const int sbExtent = m_pdfScroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int viewportW = m_pdfScroll->viewport()->width() - sbExtent - kPdfSideMargin;
    const double baseW = qMax(120, viewportW);

    // Preserve the scroll position as a fraction of the total range so a zoom or
    // resize keeps roughly the same part of the document in view.
    QScrollBar *vbar = m_pdfScroll->verticalScrollBar();
    const double ratio = vbar->maximum() > 0
                             ? double(vbar->value()) / double(vbar->maximum())
                             : 0.0;

    for (int i = 0; i < m_pdfPageLabels.size(); ++i) {
        const double Wp = qMax(1.0, m_pdfPageSizes[i].width());
        const double Hp = qMax(1.0, m_pdfPageSizes[i].height());
        const double targetW = baseW * m_pdfZoom;
        const QSize fitted(qRound(targetW), qRound(targetW * Hp / Wp));
        m_pdfPageLabels[i]->setFixedSize(fitted);
        m_pdfRenderedWidth[i] = -1; // force a re-render at the new width
        m_pdfPageLabels[i]->clear();
    }
    m_pdfContainer->adjustSize(); // recompute the scrollbar range for the new sizes

    if (ratio > 0.0 && vbar->maximum() > 0)
        vbar->setValue(qRound(ratio * vbar->maximum()));
}

void QuickView::renderVisiblePdfPages() {
    if (!m_pdfDoc || m_pdfPageLabels.isEmpty())
        return;

    // Visible window in container coordinates, padded by one viewport height above
    // and below so scrolling reveals already-rendered pages instead of blanks.
    const int top = m_pdfScroll->verticalScrollBar()->value();
    const int vh = m_pdfScroll->viewport()->height();
    const int keepTop = top - vh;
    const int keepBottom = top + 2 * vh;

    int firstVisible = -1;
    for (int i = 0; i < m_pdfPageLabels.size(); ++i) {
        QLabel *label = m_pdfPageLabels[i];
        const QRect g = label->geometry();
        const bool inWindow = g.bottom() >= keepTop && g.top() <= keepBottom;
        // The first page overlapping the actual visible band drives "page N / M".
        if (firstVisible < 0 && g.bottom() >= top && g.top() <= top + vh)
            firstVisible = i;

        if (inWindow) {
            if (m_pdfRenderedWidth[i] == label->width())
                continue; // already rendered at this width
            std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(i));
            if (!page) {
                label->setText(tr("Failed to render page %1").arg(i + 1));
                continue;
            }
            const double Wp = qMax(1.0, m_pdfPageSizes[i].width());
            const double dpi = kPdfBaseDpi * (double(label->width()) / Wp);
            const QImage image = page->renderToImage(dpi, dpi);
            if (image.isNull()) {
                label->setText(tr("Failed to render page %1").arg(i + 1));
                continue;
            }
            label->setPixmap(QPixmap::fromImage(image));
            m_pdfRenderedWidth[i] = label->width();
        } else if (m_pdfRenderedWidth[i] != -1) {
            // Far offscreen: drop the pixmap to bound memory, but keep the fixed
            // size so the layout and scrollbar range stay stable.
            label->clear();
            m_pdfRenderedWidth[i] = -1;
        }
    }

    if (firstVisible < 0)
        firstVisible = 0;
    m_pdfPageInfo->setText(
        tr("Page %1 / %2").arg(firstVisible + 1).arg(m_pdfPageLabels.size()));
}

void QuickView::closePdf() {
    // Releasing the unique_ptr frees the Poppler document; drop all page labels and
    // reset the view state so a later PDF starts clean and no stale page lingers.
    m_pdfDoc.reset();
    if (m_pdfContainer) {
        if (auto *lay = m_pdfContainer->layout()) {
            while (QLayoutItem *item = lay->takeAt(0)) {
                if (QWidget *w = item->widget())
                    w->deleteLater();
                delete item;
            }
        }
    }
    m_pdfPageLabels.clear();
    m_pdfPageSizes.clear();
    m_pdfRenderedWidth.clear();
    m_pdfZoom = 1.0;
    if (m_pdfPageInfo)
        m_pdfPageInfo->clear();
}

void QuickView::zoomImageBy(double factor) {
    m_imageFitMode = false;
    m_imageScale = qBound(kMinScale, m_imageScale * factor, kMaxScale);
    applyImageScale();
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
    if (watched == m_mpv && event->type() == QEvent::Resize) {
        positionVideoInfoOverlay(); // keep the panel pinned to the top-right corner
        // fall through to default handling
    }
    // Resizing the PDF viewport re-fits every page to the new width; debounce so a
    // divider drag re-fits once at the end rather than on every intermediate width.
    if (m_pdfScroll && watched == m_pdfScroll->viewport() &&
        event->type() == QEvent::Resize) {
        if (m_pdfDoc)
            m_pdfRelayoutTimer->start();
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
        closePdf(); // don't keep a document loaded behind the "no preview" note
        m_infoOverlay->hide();
        m_info->setText(tr("Select a file to preview"));
        m_stack->setCurrentWidget(m_info);
        return;
    }

    // Archives: a read-only listing of their contents (a pure header scan, no
    // extraction). Checked first so an archive under the cursor shows its file
    // tree instead of falling through to a garbage text head.
    if (ArchiveHandler::isSupportedArchive(path)) {
        previewArchive(path);
        return;
    }

    if (isVideo(path)) {
        m_infoOverlay->hide(); // image overlay belongs to another page

        // Already showing/playing this exact clip? Don't reload it -- a spurious
        // re-selection of the same row shouldn't restart the decode.
        if (path == m_videoPath && m_stack->currentWidget() == m_videoPage)
            return;
        m_videoPath = path;

        // Apply the persisted preview preferences to both the core and controls
        // (block signals so seeding them doesn't re-persist or fight the core).
        const int savedVolume = m_settings.videoVolume();
        const bool savedMuted = m_settings.videoMuted();
        const double savedSpeed = m_settings.videoSpeed();

        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(savedVolume);
        m_volumeSlider->blockSignals(false);
        m_mpv->setVolume(savedVolume);

        m_muteButton->blockSignals(true);
        m_muteButton->setChecked(savedMuted);
        m_muteButton->setIcon(style()->standardIcon(
            savedMuted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
        m_muteButton->blockSignals(false);
        m_mpv->setMute(savedMuted);

        int speedIndex = m_speedCombo->findData(savedSpeed);
        if (speedIndex < 0)
            speedIndex = 0; // unknown saved speed → fall back to 1x
        m_speedCombo->blockSignals(true);
        m_speedCombo->setCurrentIndex(speedIndex);
        m_speedCombo->blockSignals(false);
        m_mpv->setSpeed(m_speedCombo->itemData(speedIndex).toDouble());

        m_progressSlider->setValue(0);
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

    if (isPdf(path)) {
        m_infoOverlay->hide(); // image overlay belongs to another page
        closePdf();            // drop any prior document before loading the new one

        // Poppler::Document::load returns an owning pointer (nullptr on failure).
        std::unique_ptr<Poppler::Document> doc(Poppler::Document::load(path));
        if (!doc || doc->isLocked()) {
            // Encrypted or unreadable PDFs fall back to the info page rather than
            // showing a blank pane.
            m_info->setText(tr("Cannot open PDF: %1").arg(info.fileName()));
            m_stack->setCurrentWidget(m_info);
            return;
        }
        // Smooth glyph/vector edges; cheap and greatly improves legibility.
        doc->setRenderHint(Poppler::Document::Antialiasing, true);
        doc->setRenderHint(Poppler::Document::TextAntialiasing, true);

        m_pdfDoc = std::move(doc);
        m_pdfZoom = 1.0;
        // Switch first so the scroll viewport has its real width before we fit
        // pages to it; loadPdfPages still guards a minimum for the un-laid-out case.
        m_stack->setCurrentWidget(m_pdfPage);
        loadPdfPages();
        return;
    }

    // Reaching here means the target is not a PDF: release any loaded document so
    // we don't hold a large PDF in memory behind an image/text/markdown preview.
    closePdf();

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
            // Cap the read so a pathologically large .md can't stall the render.
            const QByteArray data = mdFile.read(kMarkdownMaxBytes);
            // QTextEdit::setMarkdown takes no dialect argument; go through the
            // document to request the GitHub dialect (tables, task lists, ...).
            m_markdown->document()->setMarkdown(QString::fromUtf8(data),
                                                QTextDocument::MarkdownDialectGitHub);
            m_stack->setCurrentWidget(m_markdown);
            return;
        }
        // Unreadable: fall through to the generic text/no-preview handling below.
    }

    if (ImageViewer::isImage(path)) {
        QImageReader reader(path);
        const QSize dim = reader.size();
        const QString format = QString::fromLatin1(reader.format()).toUpper();
        QPixmap pm(path);
        if (!pm.isNull()) {
            m_originalPixmap = pm;
            m_imagePath = path;
            loadImageSiblings(); // enable prev/next among sibling images

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
        m_textRaw = file.read(m_textCap);
        m_textTruncated = !file.atEnd();
        renderText();
        m_stack->setCurrentWidget(m_textPage);
        return;
    }

    m_info->setText(tr("No preview available for %1").arg(info.fileName()));
    m_stack->setCurrentWidget(m_info);
}
