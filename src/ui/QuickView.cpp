#include "QuickView.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QBuffer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QMessageBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <poppler-qt5.h>

#include "ArchiveHandler.h"
#include "ArchiveModel.h"
#include "ImageViewer.h"
#include "MpvWidget.h"
#include "OfficeConverter.h"
#include "config/Settings.h"

namespace {
constexpr qint64 kTextPreviewBytes = 64 * 1024;   // cap text previews at 64 KiB
constexpr qint64 kMarkdownMaxBytes = 2 * 1024 * 1024; // cap markdown at 2 MiB
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
    m_stack->addWidget(m_info);              // 0
    m_stack->addWidget(buildImagePage());    // 1
    m_stack->addWidget(m_text);              // 2
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
    auto *layout = new QVBoxLayout(m_encryptedPage);
    layout->addStretch(1);
    m_encryptedLabel = new QLabel(m_encryptedPage);
    m_encryptedLabel->setAlignment(Qt::AlignCenter);
    m_encryptedLabel->setWordWrap(true);
    layout->addWidget(m_encryptedLabel);
    m_unlockButton = new QPushButton(tr("Unlock with password…"), m_encryptedPage);
    m_unlockButton->setFixedWidth(200);
    layout->addWidget(m_unlockButton, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    connect(m_unlockButton, &QPushButton::clicked, this, &QuickView::promptAndDecrypt);
    return m_encryptedPage;
}

void QuickView::promptAndDecrypt() {
    if (m_encryptedPath.isEmpty())
        return;
    bool ok = false;
    const QString password = QInputDialog::getText(
        this, tr("Password required"),
        tr("Enter the password for “%1”:").arg(QFileInfo(m_encryptedPath).fileName()),
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;

    // Decrypt into a fresh temp dir (auto-removed with this view), keeping the
    // original file name so the extension still drives the preview type.
    m_decryptDir = std::make_unique<QTemporaryDir>();
    if (!m_decryptDir->isValid()) {
        QMessageBox::warning(this, tr("Decrypt"), tr("Could not create a temporary file."));
        return;
    }
    const QString out = QDir(m_decryptDir->path()).filePath(QFileInfo(m_encryptedPath).fileName());
    QString error;
    const OfficeConverter::DecryptStatus st =
        OfficeConverter::decrypt(m_encryptedPath, password, out, &error);
    switch (st) {
    case OfficeConverter::DecryptStatus::Ok:
        showFile(out); // preview the decrypted copy
        break;
    case OfficeConverter::DecryptStatus::WrongPassword:
        QMessageBox::warning(this, tr("Decrypt"), tr("Incorrect password."));
        break;
    case OfficeConverter::DecryptStatus::Unavailable:
        QMessageBox::warning(
            this, tr("Decrypt"),
            tr("Decryption needs python3 with the msoffcrypto module:\n"
               "pip install --user msoffcrypto-tool"));
        break;
    case OfficeConverter::DecryptStatus::Failed:
        QMessageBox::warning(this, tr("Decrypt"),
                             tr("Decryption failed: %1").arg(error));
        break;
    }
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

void QuickView::onArchiveActivated(const QModelIndex &index) {
    if (m_archiveModel->isParentEntry(index.row())) {
        navigateArchiveUp();
        return;
    }
    const auto node = m_archiveModel->nodeAt(index.row());
    if (node && node->isDir) {
        m_archiveModel->enterDirectory(node->fullPath);
        updateArchivePathLabel();
    }
}

void QuickView::navigateArchiveUp() {
    if (m_archiveModel->navigateUp())
        updateArchivePathLabel();
}

void QuickView::updateArchivePathLabel() {
    m_archivePathLabel->setText(QStringLiteral("/%1").arg(m_archiveModel->currentPath()));
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

QWidget *QuickView::buildPdfPage() {
    m_pdfPage = new QWidget(this);

    // A toolbar mirroring the image page: page navigation on the left, zoom on
    // the right, with the "page N / M" label between them.
    auto *toolbar = new QToolBar(m_pdfPage);
    toolbar->addAction(tr("Prev"), this, [this]() {
        if (m_pdfDoc && m_pdfPageIndex > 0) {
            --m_pdfPageIndex;
            renderPdfPage();
        }
    });
    toolbar->addAction(tr("Next"), this, [this]() {
        if (m_pdfDoc && m_pdfPageIndex + 1 < m_pdfDoc->numPages()) {
            ++m_pdfPageIndex;
            renderPdfPage();
        }
    });

    m_pdfPageInfo = new QLabel(m_pdfPage);
    m_pdfPageInfo->setContentsMargins(8, 0, 8, 0);
    toolbar->addWidget(m_pdfPageInfo);

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    toolbar->addAction(tr("Zoom In"), this, [this]() {
        if (!m_pdfDoc)
            return;
        m_pdfZoom = qBound(kPdfMinZoom, m_pdfZoom * kZoomStep, kPdfMaxZoom);
        renderPdfPage();
    });
    toolbar->addAction(tr("Zoom Out"), this, [this]() {
        if (!m_pdfDoc)
            return;
        m_pdfZoom = qBound(kPdfMinZoom, m_pdfZoom / kZoomStep, kPdfMaxZoom);
        renderPdfPage();
    });

    m_pdfLabel = new QLabel(m_pdfPage);
    m_pdfLabel->setAlignment(Qt::AlignCenter);
    m_pdfScroll = new QScrollArea(m_pdfPage);
    m_pdfScroll->setWidget(m_pdfLabel);
    m_pdfScroll->setWidgetResizable(false);
    m_pdfScroll->setAlignment(Qt::AlignCenter);

    auto *layout = new QVBoxLayout(m_pdfPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_pdfScroll, 1);
    return m_pdfPage;
}

void QuickView::renderPdfPage() {
    if (!m_pdfDoc)
        return;
    const int pageCount = m_pdfDoc->numPages();
    if (pageCount <= 0)
        return;
    m_pdfPageIndex = qBound(0, m_pdfPageIndex, pageCount - 1);

    // Poppler::Document::page returns an owning pointer; wrap it so it frees on
    // every path out of this function.
    std::unique_ptr<Poppler::Page> page(m_pdfDoc->page(m_pdfPageIndex));
    if (!page) {
        m_pdfLabel->setText(tr("Failed to render page %1").arg(m_pdfPageIndex + 1));
        return;
    }

    const double dpi = kPdfBaseDpi * m_pdfZoom;
    const QImage image = page->renderToImage(dpi, dpi);
    if (image.isNull()) {
        m_pdfLabel->setText(tr("Failed to render page %1").arg(m_pdfPageIndex + 1));
        return;
    }
    m_pdfLabel->setPixmap(QPixmap::fromImage(image));
    m_pdfLabel->resize(image.size());
    m_pdfPageInfo->setText(tr("Page %1 / %2").arg(m_pdfPageIndex + 1).arg(pageCount));
}

void QuickView::closePdf() {
    // Releasing the unique_ptr frees the Poppler document; reset the view state
    // so a later PDF starts clean and no stale page lingers.
    m_pdfDoc.reset();
    m_pdfPageIndex = 0;
    m_pdfZoom = 1.0;
    if (m_pdfLabel)
        m_pdfLabel->clear();
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
        stopVideo();
        closePdf();
        m_infoOverlay->hide();
        QString err;
        if (m_archiveModel->loadArchive(path, &err)) {
            updateArchivePathLabel();
            m_stack->setCurrentWidget(m_archivePage);
        } else {
            m_info->setText(tr("Cannot open archive: %1").arg(info.fileName()));
            m_stack->setCurrentWidget(m_info);
        }
        return;
    }

    if (isVideo(path)) {
        m_infoOverlay->hide(); // image overlay belongs to another page

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
        m_pdfPageIndex = 0;
        m_pdfZoom = 1.0;
        renderPdfPage();
        m_stack->setCurrentWidget(m_pdfPage);
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
        const OfficeConverter::Result r = OfficeConverter::convert(path);
        if (r.ok && r.kind == OfficeConverter::Kind::Document) {
            // Fit large embedded images to the preview width. Use the stack's
            // width (the actual pane width) rather than m_markdown's, which may
            // still be stale until it's shown as the current page below.
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
        if (r.encrypted) {
            m_encryptedPath = path;
            const bool canDec = OfficeConverter::canDecrypt();
            m_encryptedLabel->setText(canDec
                                          ? tr("“%1” is encrypted.").arg(info.fileName())
                                          : tr("“%1” is encrypted and cannot be previewed.")
                                                .arg(info.fileName()));
            m_unlockButton->setVisible(canDec);
            m_stack->setCurrentWidget(m_encryptedPage);
            return;
        }
        m_info->setText(tr("Cannot preview %1:\n%2").arg(info.fileName(), r.error));
        m_stack->setCurrentWidget(m_info);
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
