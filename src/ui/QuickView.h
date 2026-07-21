#pragma once

#include <QPixmap>
#include <QPoint>
#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTableWidget;
class QTextBrowser;
class QTimer;
class MpvWidget;
class Settings;

// Poppler's document type is only referenced through a unique_ptr member, so a
// forward declaration keeps the heavy poppler-qt5.h out of this header.
namespace Poppler {
class Document;
}

// Lightweight in-panel preview shown by Ctrl+Q: renders the file under the
// cursor as a zoomable image, a text head, or a "no preview" note.
class QuickView : public QWidget {
    Q_OBJECT

public:
    explicit QuickView(Settings &settings, QWidget *parent = nullptr);
    // Out-of-line so the unique_ptr<Poppler::Document> member can be destroyed
    // where the complete Poppler type is visible (it is only forward-declared
    // above).
    ~QuickView() override;

    void showFile(const QString &path);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    static bool isVideo(const QString &path);
    static bool isMarkdown(const QString &path);
    static bool isPdf(const QString &path);

private:
    QWidget *buildImagePage();
    QWidget *buildVideoPage();
    QWidget *buildMarkdownPage();
    QWidget *buildPdfPage();
    QWidget *buildOfficeTablePage();       // spreadsheet (xls/xlsx) preview as a grid
    void populateCsvTable(const QString &csv); // fill the office table from CSV text
    // Adds width/height attributes to office HTML <img> tags whose natural width
    // exceeds maxWidth, so large embedded images fit the preview pane instead of
    // overflowing (QTextBrowser ignores CSS max-width).
    QString fitImagesToWidth(const QString &html, int maxWidth) const;
    void renderPdfPage(); // (re)render the current PDF page at the current zoom
    void closePdf();      // release any loaded document + reset PDF UI state
    void applyImageScale();
    void zoomImageBy(double factor);
    double fitScale() const;
    void positionInfoOverlay();      // keep the image metadata panel pinned top-right
    void positionVideoInfoOverlay(); // same, over the video area
    void updateVideoInfoOverlay();   // refresh the video metadata text
    void stopVideo();                // unload + hide the video page

    QStackedWidget *m_stack;
    QWidget *m_imagePage;
    QScrollArea *m_imageScroll;
    QLabel *m_imageLabel;
    QCheckBox *m_lockZoomCheck; // when checked, keep the ratio for later images
    QCheckBox *m_infoCheck;     // when checked, overlay image metadata
    QLabel *m_infoOverlay;      // floating metadata panel over the image
    QTimer *m_refitTimer;       // coalesces refits during interactive resize
    QPlainTextEdit *m_text;
    QLabel *m_info;

    // Markdown page (m_stack index 4): a rich-text browser that renders the
    // file via QTextDocument's bundled MD4C support (Qt 5.14+), no extra deps.
    QTextBrowser *m_markdown = nullptr;

    // Office spreadsheet page: a read-only grid populated from office_oxide's CSV
    // output. Word/PowerPoint documents reuse the markdown page above.
    QTableWidget *m_officeTable = nullptr;

    // PDF page (m_stack index 5): a single rendered page in a scroll area with
    // prev/next + zoom controls. Poppler renders each page to a QImage on demand.
    QWidget *m_pdfPage = nullptr;
    QScrollArea *m_pdfScroll = nullptr;
    QLabel *m_pdfLabel = nullptr;
    QLabel *m_pdfPageInfo = nullptr;           // "page N / M"
    std::unique_ptr<Poppler::Document> m_pdfDoc; // currently loaded document
    int m_pdfPageIndex = 0;                    // 0-based page currently shown
    double m_pdfZoom = 1.0;                     // render scale; 1.0 == 72 dpi

    // Video page (m_stack index 3).
    QWidget *m_videoPage = nullptr;
    MpvWidget *m_mpv = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_muteButton = nullptr; // checkable: checked == muted
    QComboBox *m_speedCombo = nullptr;
    QSlider *m_progressSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QCheckBox *m_videoInfoCheck = nullptr;
    QLabel *m_videoInfoOverlay = nullptr;
    QTimer *m_videoTimer = nullptr; // polls position while playing
    bool m_seeking = false;         // suppress timer updates while dragging

    Settings &m_settings; // persisted video speed / volume / mute

    QPixmap m_originalPixmap;
    double m_imageScale = 1.0;
    bool m_imageFitMode = true; // re-fit on resize until the user zooms

    // Left-drag panning state.
    bool m_panning = false;
    QPoint m_panStart;
    int m_panHScroll = 0;
    int m_panVScroll = 0;
};
