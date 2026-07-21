#pragma once

#include <QByteArray>
#include <QPixmap>
#include <QPoint>
#include <QStringList>
#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTableView;
class QToolBar;
class QTableWidget;
class QTemporaryDir;
class QTextBrowser;
class QTimer;
class ArchiveModel;
class MpvWidget;
class Settings;

// Poppler's document type is only referenced through a unique_ptr member, so a
// forward declaration keeps the heavy poppler-qt5.h out of this header.
namespace Poppler {
class Document;
}

// Multi-page file preview: images (zoom/pan), text, video, PDF, markdown,
// office documents, and archive listings. Used both as the embedded Ctrl+Q
// pane (Context::Embedded) and as the content of the top-level F3 ViewerWindow
// (Context::Window); the context tunes the text read cap and which controls
// show, so both surfaces share one implementation.
class QuickView : public QWidget {
    Q_OBJECT

public:
    enum class Context { Embedded, Window };

    explicit QuickView(Settings &settings, Context context = Context::Embedded,
                       QWidget *parent = nullptr);
    // Out-of-line so the unique_ptr<Poppler::Document> member can be destroyed
    // where the complete Poppler type is visible (it is only forward-declared
    // above).
    ~QuickView() override;

    void showFile(const QString &path);

    // Formats raw bytes as an offset/hex/ascii dump (the Hex toggle). Static so
    // it can be unit-tested without a widget.
    static QString toHexDump(const QByteArray &data);

    // Driven by the F3 ViewerWindow's host-level shortcuts; no-ops unless the
    // relevant page is current.
    void findNext();          // F3: next match in the text page
    void showPrevSibling();   // Left: previous image in the same directory
    void showNextSibling();   // Right: next image in the same directory

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    static bool isVideo(const QString &path);
    static bool isMarkdown(const QString &path);
    static bool isPdf(const QString &path);

private:
    QWidget *buildImagePage();
    QWidget *buildTextPage();     // toolbar (encoding/hex/wrap/find) + the editor
    void renderText();            // (re)render m_textRaw per the encoding/hex toggle
    void loadImageSiblings();     // list sibling images in the current dir
    QWidget *buildVideoPage();
    QWidget *buildMarkdownPage();
    QWidget *buildPdfPage();
    QWidget *buildOfficeTablePage();       // spreadsheet (xls/xlsx) preview as a grid
    QWidget *buildEncryptedPage();         // "encrypted" note + an unlock button
    QWidget *buildArchivePage();           // read-only archive listing (no extraction)
    void onArchiveActivated(const QModelIndex &index); // enter dir / go up
    void navigateArchiveUp();
    void updateArchivePathLabel();
    void populateCsvTable(const QString &csv); // fill the office table from CSV text
    // Prompts for a password and, on success, decrypts m_encryptedPath to a temp
    // file and previews the decrypted result. Wired to the unlock button.
    void promptAndDecrypt();
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

    // Text page extras. The encoding/hex/wrap/find toolbar shows only in the F3
    // Window context; the embedded pane keeps a clean plaintext head.
    QWidget *m_textPage = nullptr;
    QToolBar *m_textToolbar = nullptr;
    QComboBox *m_textEncoding = nullptr;
    QLineEdit *m_textFind = nullptr;
    QByteArray m_textRaw;              // raw bytes of the current text file
    bool m_textHex = false;           // hex-dump mode
    bool m_textTruncated = false;     // the read hit the cap
    qint64 m_textCap = 0;             // max bytes read (context-dependent)

    // Image sibling navigation (prev/next among images in the same directory).
    QString m_imagePath;
    QStringList m_imageSiblings;
    int m_imageSiblingIndex = -1;

    // Markdown page (m_stack index 4): a rich-text browser that renders the
    // file via QTextDocument's bundled MD4C support (Qt 5.14+), no extra deps.
    QTextBrowser *m_markdown = nullptr;

    // Office spreadsheet page: a read-only grid populated from office_oxide's CSV
    // output. Word/PowerPoint documents reuse the markdown page above.
    QTableWidget *m_officeTable = nullptr;

    // Encrypted-office page: a note plus an "unlock" button that prompts for the
    // password and previews the decrypted copy.
    QWidget *m_encryptedPage = nullptr;
    QLabel *m_encryptedLabel = nullptr;
    QPushButton *m_unlockButton = nullptr;
    QString m_encryptedPath;                      // file awaiting a password
    std::unique_ptr<QTemporaryDir> m_decryptDir;  // holds the decrypted temp file

    // Archive page: a read-only listing of an archive's entries (a pure header
    // scan via ArchiveModel -- nothing is extracted). Activate-to-navigate like
    // the file panels; "Up" climbs out of a subdirectory.
    QWidget *m_archivePage = nullptr;
    QTableView *m_archiveView = nullptr;
    ArchiveModel *m_archiveModel = nullptr;
    QLabel *m_archivePathLabel = nullptr;

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
    Context m_context;    // Embedded (Ctrl+Q pane) vs Window (F3 viewer)

    QPixmap m_originalPixmap;
    double m_imageScale = 1.0;
    bool m_imageFitMode = true; // re-fit on resize until the user zooms

    // Left-drag panning state.
    bool m_panning = false;
    QPoint m_panStart;
    int m_panHScroll = 0;
    int m_panVScroll = 0;
};
