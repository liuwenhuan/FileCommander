#pragma once

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QSizeF>
#include <QStyle>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <QWidget>

#include "AnimatedImage.h"

#include <memory>

#include "ArchiveHandler.h" // ArchiveNode + ArchiveHandler::Status (async load result)
#include "ImagePreviewLoader.h"
#include "OfficeConverter.h" // OfficeConverter::Result (async conversion payload)
#include "TextEncodingDetector.h"
#include "media/MediaEngine.h"

class QCheckBox;
class QComboBox;
class QGraphicsItem;
class QGraphicsPixmapItem;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QProgressBar;
class QPropertyAnimation;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTableView;
class QTemporaryDir;
class QToolBar;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QTextEdit;
class QTimer;
class ArchiveModel;
class Settings;
class TextEditor;

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

    // Softens the one piece of Markdown Qt's parser mishandles badly enough to
    // lose the document: a <br> inside a TABLE ROW.
    //
    // Qt renders Markdown with its bundled MD4C, which treats <br> as the start
    // of inline HTML and swallows the rest of the cell -- and with it every
    // remaining cell, every remaining row, and (measured on a real 82 KB file)
    // all 62 tables after it. Two <br> tags emptied 179 of 204 cells.
    //
    // Rows only. Whether <br> misbehaves in prose is untested, and rewriting
    // text nobody complained about is not something a previewer should do.
    //
    // Public because it is worth testing on its own; the caller is the markdown
    // load path.
    static QString softenTableLineBreaks(const QString &markdown);
    enum class Context { Embedded, Window };

    explicit QuickView(Settings &settings, Context context = Context::Embedded,
                       QWidget *parent = nullptr,
                       std::unique_ptr<MediaEngine> mediaEngine = {});
    // Out-of-line so the unique_ptr<Poppler::Document> member can be destroyed
    // where the complete Poppler type is visible (it is only forward-declared
    // above).
    ~QuickView() override;

    // Builds the video page without a clip, warming the media engine first
    // because the page wires itself to it. The page is otherwise created only
    // on the first video preview, which a test about the transport's LAYOUT has
    // no business requiring. Null if no engine could be made.
    QWidget *buildVideoPageForTest();

    // Spins until a pending text probe has landed. The text preview reads and
    // sniffs off-thread, so a test that asserts on the editor straight after
    // showFile() would be asserting on the file before it.
    bool waitForTextIdleForTest(int timeoutMs = 5000);

    // Current quarter-turn applied to the video, for tests and for the toolbar.
    int videoRotation() const { return m_videoRotation; }

    void showFile(const QString &path);
    // Initializes the configured media backend once. MainWindow calls this after
    // first paint; a media request calls it immediately when it wins that race.
    // Audio/video controls remain lazy until their own first use.
    void warmMediaEngine();

    // Heavy document pages are created only when their content type is first
    // needed. Each accessor is idempotent.
    QWidget *ensurePdfPage();
    QWidget *ensureOfficePage();
    QWidget *ensureArchivePage();
    QWidget *ensureSlidesPage();

    // Shows approved static content with the shared fast opacity reveal. Media,
    // progress, encrypted-control, and native/OpenGL surfaces are shown directly.
    void revealStaticPage(QWidget *page);

    // Network-preview download states. A remote file must be fetched to a local
    // temp file before it can be previewed; while that runs, the preview pane
    // shows this download page (message + progress + a Stop button) so the user
    // isn't left staring at a blank pane and can abort a large download.
    void showDownloading(const QString &name);           // "downloading to preview…"
    // Same waiting page, different reason and no Stop button: extracting an
    // entry out of an archive is not interruptible, so offering to cancel it
    // would be a dead control.
    void showPreparing(const QString &name);
    void setDownloadProgress(qint64 done, qint64 total); // update the bar
    void showDownloadCancelled(const QString &name);     // "preview cancelled by user"

    // Halts any active media playback (video and audio). Called when the preview
    // pane is dismissed (Ctrl+Q) so a clip doesn't keep playing while hidden.
    void stopPlayback();

signals:
    // Emitted once after the backend is ready. The host uses it to cancel a
    // still-pending post-paint warm-up.
    void mediaEngineWarmed(qint64 elapsedMs);

    // Emitted once when backend initialization fails. The failure remains
    // stable for this QuickView; restarting the application creates a new
    // backend and is the explicit retry boundary.
    void mediaEngineWarmFailed(const QString &message);

    // The Stop button on the download page was clicked: the host should cancel
    // the in-flight remote download for the current preview.
    void downloadCancelRequested();

    // A remote file handed over as a stream URL could not be played. The host
    // should fall back to downloading it, which is what happened before
    // streaming existed.
    void streamFailed(const QString &url);

    // The Edit button on the text toolbar was pressed for this file. The host
    // resolves the path -- only it knows whether the file is editable in place
    // (a downloaded copy is not) -- and then calls beginEditing().
    void editRequested(const QString &path);

    // The stack moved into or out of the editor page. The host uses it to keep
    // its own preview-only shortcuts off the editor's keystrokes.
    void editingChanged(bool editing);

public:
    // Hides the text toolbar's Edit button. Set for a preview opened on a
    // downloaded copy, where saving would write to a temp file the user never
    // sees again.
    void setTextEditingEnabled(bool enabled);

    // Swaps the stack to a full TextEditor on `path`, in place: no second
    // window opens, and the Preview button on the editor's toolbar brings this
    // same widget back to the text page. Returns false if the file could not be
    // loaded, leaving the current page alone.
    bool beginEditing(const QString &path);
    bool isEditing() const;
    // Asks about an unsaved buffer on behalf of the editor page, which -- being
    // a child widget rather than a window -- never receives a close event.
    // Returns false if the user chose Cancel.
    bool confirmDiscardEdits();

    // Sets the point size of the text-preview font, so the preview tracks the
    // app's file-list font-size setting. Applies to the plain-text/hex page and
    // the markdown/office rich-text page; the monospace text page keeps its
    // family, only the size changes.
    void setContentFontSize(int pt);

    // Changes the proportional content family without overriding the text page's
    // dedicated monospace family.
    void setContentFontFamily(const QString &family);

    // Applies the interface font to this preview's CHROME -- the per-page
    // toolbars and everything docked into them (encoding combo, page readout,
    // checkboxes). Deliberately not the previewed content, which follows the
    // separate file-list font size through setContentFontSize(): a whole-tree
    // font pass here would overwrite it.
    void applyChromeFont(const QFont &font);

    // Moves keyboard focus into the preview's current page (its primary
    // interactive widget). Used when the user Tabs from the file list into the
    // embedded preview pane, so the pane — not the panel it covers — takes focus.
    void focusPreview();

    // Formats raw bytes as an offset/hex/ascii dump (the Hex toggle). Static so
    // it can be unit-tested without a widget.
    static QString toHexDump(const QByteArray &data);

    // Whether a file of this name can be previewed straight off a network
    // backend, without being downloaded first. The host asks before deciding
    // between a stream URL and a temp-file download.
    static bool canStreamPreview(const QString &path);

    // Driven by the F3 ViewerWindow's host-level shortcuts; no-ops unless the
    // relevant page is current.
    void findNext();          // F3: next match in the text page
    void showPrevSibling();   // Left: previous image in the same directory
    void showNextSibling();   // Right: next image in the same directory

    // Moves playback `seconds` forwards (or backwards, when negative) on the
    // video/audio page. Returns false -- taking no action -- when no media page
    // is current, so the caller can leave the key to whatever else wants it.
    bool nudgeMediaPosition(int seconds);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    static bool isVideo(const QString &path);
    static bool isMarkdown(const QString &path);
    static bool isPdf(const QString &path);
    static bool isAudio(const QString &path);

private:
    bool isStaticPageEligible(QWidget *page) const;
    QWidget *staticContentTarget(QWidget *page) const;
    void finishStaticReveal();
    void releaseHiddenDocumentPages(QWidget *page);
    void cancelPendingPreviewWork();
    QWidget *buildImagePage();
    QWidget *buildTextPage();     // toolbar (encoding/hex/wrap/find) + the editor
    void renderText();            // (re)render m_textRaw per the encoding/hex toggle
    void loadImageSiblings();     // list sibling images in the current dir
    QWidget *buildVideoPage();
    // Adds `degrees` to the current quarter turn and pushes it to the engine.
    void rotateVideoBy(int degrees);
    QWidget *ensureVideoPage();
    QWidget *buildAudioPage();
    QWidget *ensureAudioPage();
    void showMediaEngineFailure();
    void showAudio(const QString &path);  // load + populate the audio page
    void loadAudioSiblings();             // list sibling audio files in the dir
    void stopAudio();                     // halt playback + timer
    void updateAudioTransport();          // sync play/pause label + seek + times
    QIcon mediaIcon(QStyle::StandardPixmap standardPixmap) const;
    void refreshMediaControlIcons();
    QWidget *buildMarkdownPage();
    // Reads, parses and lays out a .md file on a worker thread, then installs the
    // finished QTextDocument into m_markdown (keeps the GUI responsive on large or
    // table-heavy files). Stale renders are dropped via m_markdownGen.
    void loadMarkdownAsync(const QString &path);
    void renderMarkdown();        // (re)decode m_markdownRaw per the encoding chooser
    QWidget *buildPdfPage();
    QWidget *buildSlidesPage();            // pptx slide-image preview (per-slide SVG)
    QWidget *buildOfficeTablePage();       // spreadsheet (xls/xlsx) preview as a grid
    QWidget *buildEncryptedPage();         // "encrypted" note + an unlock button
    QWidget *buildDownloadPage();          // remote-preview download status + Stop
    QWidget *buildArchivePage();           // read-only archive listing (no extraction)
    void previewArchive(const QString &path);   // start a fresh archive chain at path
    void tryLoadCurrentArchive();               // (re)load the current chain level (async)
    void descendIntoNestedArchive(const QString &entryFullPath, const QString &entryName);
    // Smart preview drill-down: when a freshly-loaded archive level holds exactly
    // one entry, follow it automatically -- step into a lone sub-directory, or
    // extract + open a lone nested archive -- and repeat, until the level branches
    // (more than one entry) or a single plain file is reached. Mirrors smart
    // extraction's "skip pointless wrapper folders" behaviour. Only runs while
    // m_archiveAutoDescend is set (a fresh preview); any manual navigation clears
    // it so the user keeps control.
    void autoDescendArchive();
    // Result of a worker-thread buildTree; handled back on the GUI thread.
    struct ArchiveLoadResult {
        QSharedPointer<ArchiveNode> root;
        ArchiveHandler::Status status = ArchiveHandler::Status::Ok;
        QString err;
        QString packageInfo; // .deb control / .rpm header text, empty otherwise
    };
    void handleArchiveLoad(const ArchiveLoadResult &r, const QString &path, const QString &pw,
                           qint64 size, qint64 mtime);
    void onArchiveActivated(const QModelIndex &index); // enter dir / descend / go up
    void navigateArchiveUp();
    void updateArchivePathLabel();
    // Show `info` (a .deb control file / .rpm header block) in the panel above the
    // tree, or hide the panel when `info` is empty.
    void setArchivePackageInfo(const QString &info);
    // Fill the office preview with one grid tab per worksheet (name, TSV).
    void populateSheets(const QVector<QPair<QString, QString>> &sheets);
    // The QTableWidget of the currently selected worksheet tab, or null.
    QTableWidget *currentOfficeTable() const;
    // Converts an office file (optionally with a password) OFF the GUI thread and
    // shows the result: the rendered document/grid, the inline password page
    // (encrypted / wrong password / unsupported), or an error. Called with an empty
    // password from showFile() and with the typed password from tryUnlock(). The
    // conversion (subprocess + JSON parse) can take a second or more on a big deck,
    // so it runs via QtConcurrent and the result is applied by handleOfficeResult()
    // on the GUI thread; a generation counter drops results superseded by a newer
    // selection.
    void renderOffice(const QString &path, const QString &password); // debounced entry point
    void startOfficeRender(const QString &path, const QString &password); // runs the actual convert
    void handleOfficeResult(const OfficeConverter::Result &r, const QString &path);
    // Reads the inline password field and re-renders m_encryptedPath with it,
    // giving feedback in place on a wrong password. Wired to the unlock button and
    // the field's returnPressed.
    void tryUnlock();
    // Adds width/height attributes to office HTML <img> tags whose natural width
    // exceeds maxWidth, so large embedded images fit the preview pane instead of
    // overflowing (QTextBrowser ignores CSS max-width).
    QString fitImagesToWidth(const QString &html, int maxWidth) const;
    // Continuous PDF preview: all pages stack vertically in one scroll area,
    // fit-to-width by default and rendered lazily (only pages near the viewport).
    void loadPdfPages();          // build one placeholder label per page of m_pdfDoc
    void relayoutPdfPages();      // recompute every label's fitted size, force re-render
    void renderVisiblePdfPages(); // render pixmaps for on-screen pages, free far ones
    void closePdf();              // release any loaded document + reset PDF UI state
    // Continuous pptx slide preview: every slide's SVG stacks vertically in one
    // scroll area, fit-to-width and rendered lazily (mirrors the PDF cluster, with
    // QSvgRenderer standing in for Poppler).
    void loadSlides(const QStringList &svgs); // parse every slide's SVG into the scene
    // Two-stage load: append the slides beyond the ones already shown (the full
    // deck arriving after the fast first-N paint), extending the scene downward
    // without disturbing the current scroll position or the loaded slides.
    void appendRemainingSlides(const QStringList &fullSvgs);
    // Append slides from index `from` in small chunks across event-loop turns (keyed
    // to the office gen so a file switch abandons a half-built deck), so a large deck
    // doesn't block the UI building every placeholder + parsing metadata at once.
    void appendSlidesChunk(const QStringList &fullSvgs, int from, int gen);
    void relayoutSlides();        // recompute the fit-to-width view transform
    void renderVisibleSlides();   // build on-screen slides, free far ones, update readout
    void buildSlideItem(int i);   // replace slide i's placeholder with its full item tree
    void releaseSlideItem(int i); // swap slide i's full item tree back for a placeholder
    void closeSlides();           // drop all slide data + reset the slides UI state
    void buildPdfPageText(int i); // build the transparent selectable text layer for one page
    // Copies text to the clipboard for the PDF/slides pages. Prefers whatever the
    // user has selected in the scene; scope picks the fallback when nothing is
    // selected (current page under the scroll position, or the whole document).
    enum class CopyScope { Selection, CurrentPage, All };
    void copyPdfText(CopyScope scope);
    void copySlidesText(CopyScope scope);
    int currentPdfPage() const;   // page index under the current scroll position
    int currentSlide() const;     // slide index under the current scroll position
    void applyImageScale();
    void requestImageRender();
    void preserveImageTransitionSnapshot();
    void clearImageTransitionSnapshot();
    void zoomImageBy(double factor, bool debounce = false);
    void invalidateImageRequests();
    QSize transformedImageSize() const;
    void updateImageInfoOverlay();
    void updateImageCursor(const QSize &displaySize);
    // Rotates the shown image by +/-90 degrees, then persists it losslessly back
    // to m_imagePath (jpegtran for JPEG when available, QImageWriter otherwise).
    void rotateCurrentImage(int degrees);
    double fitScale() const;
    void positionInfoOverlay();      // keep the image metadata panel pinned top-right
    void positionVideoInfoOverlay(); // same, over the video area
    void showVideoNotice(const QString &text); // transient banner over the video
    void applyUnknownDuration();               // a length the backend cannot report
    // Rebuilds mpv's phosphor filter chain from the current tint, pixel block
    // and the width the video is drawn at. Idempotent; safe to call often.
    void applyVideoPhosphor();

public:
    // Re-derives every preview surface from what it kept, after the theme or
    // the "tint images" setting changed. Without it the pane keeps showing the
    // treatment in force when the file was opened.
    void refreshPhosphor();

private:
    void updateVideoInfoOverlay();   // refresh the video metadata text
    void stopVideo();                // unload + hide the video page

    QStackedWidget *m_stack;
    QPropertyAnimation *m_staticRevealAnimation = nullptr;
    QPointer<QWidget> m_staticRevealTarget;

    // Download page (remote preview): a centred message, a progress bar and a
    // Stop button. Shown while a network file is being fetched to a temp file.
    QWidget *m_downloadPage = nullptr;
    QLabel *m_downloadLabel = nullptr;
    QProgressBar *m_downloadProgress = nullptr;
    QPushButton *m_downloadStopButton = nullptr;

    QWidget *m_imagePage;
    QScrollArea *m_imageScroll;
    QLabel *m_imageLabel;
    // Animation, when the file has more than one frame. Kept beside the still
    // path rather than replacing it: a still is decoded once and transformed on
    // demand, an animation is decoded continuously and its frames thrown away.
    AnimatedImage *m_animation = nullptr;
    // Frames arrive from AnimatedImage already recoloured, so the shared render
    // path must not tint them a second time.
    bool m_imageAlreadyTinted = false;
    QAction *m_imagePlayAction = nullptr;
    QList<QAction *> m_imageRotateActions;
    // Rotation for a still, play/pause for an animation -- never both.
    void showImageControlsFor(bool animated);
    void stopAnimation();
    QLabel *m_imageTransitionSnapshot = nullptr;
    QCheckBox *m_lockZoomCheck; // when checked, keep the ratio for later images
    QCheckBox *m_infoCheck;     // when checked, overlay image metadata
    QLabel *m_infoOverlay;      // floating metadata panel over the image
    QTimer *m_refitTimer;       // coalesces refits during interactive resize
    QTimer *m_imageWheelRenderTimer; // keeps the prior pixmap pending for exactly 50 ms
    QPlainTextEdit *m_text;
    QLabel *m_info;

    // Text page extras. The encoding/hex/wrap/find toolbar shows only in the F3
    // Window context; the embedded pane keeps a clean plaintext head.
    QWidget *m_textPage = nullptr;
    QToolBar *m_textToolbar = nullptr;
    QComboBox *m_textEncoding = nullptr;
    QAction *m_textEditAction = nullptr; // hands the file to the F4 editor
    QLineEdit *m_textFind = nullptr;
    QByteArray m_textRaw;              // raw bytes of the current text file
    QString m_textPath;                // keeps manual encoding only for this selection
    TextEncodingDetector::Result m_textAutoResult; // detected once per loaded file
    bool m_textAutoResultValid = false;
    bool m_textHex = false;           // hex-dump mode
    bool m_textTruncated = false;     // the read hit the cap
    qint64 m_textCap = 0;             // max bytes read (context-dependent)
    // Newest text probe. A read that lands after the cursor moved is dropped.
    quint64 m_textLoadGeneration = 0;
    bool m_textLoadPending = false;
    // In-place editing. The editor is a page of the same stack, built on first
    // use; m_textRestore* carries the scroll position across a switch in either
    // direction (both views are NoWrap, so a scrollbar value is a line number).
    TextEditor *m_editor = nullptr;
    QString m_textRestorePath;
    int m_textRestoreLine = -1;

    // Image sibling navigation (prev/next among images in the same directory).
    QString m_imagePath;
    QStringList m_imageSiblings;
    int m_imageSiblingIndex = -1;

    // Markdown page: a rich-text browser that renders the
    // file via QTextDocument's bundled MD4C support (Qt 5.14+), no extra deps.
    // Its toolbar carries the same three controls as the text page -- encoding,
    // find, edit -- and hides itself when the page is showing a converted office
    // document, which is not a file the user can re-decode or edit as text.
    QWidget *m_markdownPage = nullptr;
    QToolBar *m_markdownToolbar = nullptr;
    QComboBox *m_markdownEncoding = nullptr;
    QLineEdit *m_markdownFind = nullptr;
    QAction *m_markdownEditAction = nullptr;
    QTextBrowser *m_markdown = nullptr;
    int m_markdownGen = 0; // supersede stale async markdown renders
    QByteArray m_markdownRaw; // raw bytes, kept so a new encoding needs no re-read
    QString m_markdownPath;
    TextEncodingDetector::Result m_markdownAutoResult;
    bool m_markdownAutoResultValid = false;

    // Office spreadsheet page: a read-only grid populated from office_oxide's CSV
    // output. Word/PowerPoint documents reuse the markdown page above.
    QTabWidget *m_officeTabs = nullptr; // spreadsheet preview: one grid tab per worksheet

    // Async office conversion. m_officeGen is bumped on every file switch (and every
    // renderOffice call); a completed conversion whose captured gen no longer
    // matches is dropped, so a superseded selection never paints over the current
    // one. m_officeShownPath de-dupes a spurious re-selection of the file already
    // displayed (the embedded preview follows the file-list cursor).
    int m_officeGen = 0;
    QString m_officeShownPath;
    // Debounce rapid file switches: a fast arrow-key sweep through a folder would
    // otherwise spawn one office_oxide process per file. renderOffice() only stashes
    // the target and (re)starts this timer; the convert fires once the cursor settles.
    QTimer *m_officeConvertTimer = nullptr;
    QString m_pendingOfficePath;
    QString m_pendingOfficePassword;

    // Encrypted-office page: an inline password field (no popup) with feedback
    // shown in place. office_oxide decrypts in-process, so a correct password
    // renders the document directly; a wrong one just updates the feedback label.
    QWidget *m_encryptedPage = nullptr;
    QLabel *m_encryptedLabel = nullptr;       // "“file” is encrypted. Enter password:"
    QLineEdit *m_passwordEdit = nullptr;      // inline password entry
    // The inline password page is shared by office files and archives; the kind
    // decides what tryUnlock() does with the entered password.
    enum class EncryptedKind { Office, Archive };
    EncryptedKind m_encryptedKind = EncryptedKind::Office;
    QPushButton *m_unlockButton = nullptr;    // triggers tryUnlock()
    QLabel *m_encryptedFeedback = nullptr;    // wrong-password / error note, in place
    QString m_encryptedPath;                  // file awaiting a password
    // Widget that held keyboard focus when the encrypted page was shown (the file
    // list). Tab/Backtab in m_passwordEdit returns focus here instead of cycling
    // to the Unlock button. QPointer so it self-nulls if the widget is destroyed.
    QPointer<QWidget> m_focusBeforeEncrypted;

    // Archive page: a read-only listing of an archive's entries (a pure header
    // scan via ArchiveModel -- nothing is extracted). Activate-to-navigate like
    // the file panels; "Up" climbs out of a subdirectory.
    QWidget *m_archivePage = nullptr;
    QTableView *m_archiveView = nullptr;
    ArchiveModel *m_archiveModel = nullptr;
    QLabel *m_archivePathLabel = nullptr;
    // Package metadata panel above the tree: shows a .deb's control file or a
    // .rpm's header info. Hidden for every other archive (and for nested levels).
    QTextEdit *m_archiveInfoView = nullptr;
    // Nesting chain: paths[0] is the previewed archive, later entries are nested
    // archives the user clicked into (extracted to m_nestedDir). passwords is
    // parallel (empty until an encrypted level is unlocked). "Up" at a nested
    // root pops back to the parent archive.
    QStringList m_archivePaths;
    QStringList m_archivePasswords;
    std::unique_ptr<QTemporaryDir> m_nestedDir; // holds extracted nested archives
    bool m_archiveAutoDescend = false;          // smart drill-down active (see autoDescendArchive)

    // Async listing + cache: big solid/streaming archives (tar.bz2, 7z solid)
    // take a while to list (libarchive must decompress the stream), so buildTree
    // runs on a worker thread and populates the model here. The cache (keyed by
    // path + size + mtime) makes re-visits and "Up" instant.
    struct CachedArchive {
        QSharedPointer<ArchiveNode> root;
        QString passphrase;
        qint64 size = 0;
        qint64 mtime = 0;
        QString packageInfo; // cached .deb/.rpm metadata (empty for other archives)
    };
    int m_archiveGen = 0;                                // supersede stale async loads
    std::shared_ptr<std::atomic<bool>> m_archiveCancel; // current load's cancel flag
    QHash<QString, CachedArchive> m_archiveCache;
    QStringList m_archiveCacheOrder;                    // FIFO eviction order

    // PDF page: every page stacks vertically in one QGraphicsScene
    // and the whole document scrolls continuously. Each page is a bitmap background
    // (Poppler renderToImage -> QGraphicsPixmapItem, kept sharp by re-rendering at
    // the zoomed resolution) with a transparent, selectable text layer on top
    // (Poppler textList -> one QGraphicsTextItem per word) so the visible pixels
    // are faithful yet the text can be selected and copied. Pages fit the viewport
    // width by default; Zoom In/Out multiply that fit. Bitmaps and text are built
    // lazily (only pages near the viewport) so a 500-page PDF opens without hitching.
    QWidget *m_pdfPage = nullptr;
    QGraphicsView *m_pdfView = nullptr;
    QGraphicsScene *m_pdfScene = nullptr;
    QLabel *m_pdfPageInfo = nullptr;             // "page N / M" per scroll position
    QVector<QGraphicsPixmapItem *> m_pdfBgItems; // one background item per page (parents its text layer)
    QVector<QSizeF> m_pdfPageSizes;              // native page sizes in points (== px @ 72 dpi)
    QVector<double> m_pdfPageTop;                // scene-Y of each page's top edge
    QVector<int> m_pdfRenderedWidth;             // px width each page bitmap was rendered at (-1 == none)
    QVector<bool> m_pdfTextBuilt;                // whether a page's transparent text layer is present
    QTimer *m_pdfRelayoutTimer = nullptr;        // debounce viewport resizes before re-fitting
    int m_pdfGen = 0;                            // supersedes stale asynchronous document loads
#if FILECOMMANDER_HAS_PREVIEW_PDF
    std::shared_ptr<Poppler::Document> m_pdfDoc; // currently loaded document
#else
    bool m_pdfDoc = false; // keeps shared PDF UI guards valid in dependency-light builds
#endif
    double m_pdfZoom = 1.0;                       // user zoom multiplier on top of fit-to-width; 1.0 == fit

    // Slides page: pptx rendered by office_oxide to one
    // standalone SVG per slide. Each slide's SVG is parsed into native graphics
    // items (SlideSceneBuilder) and stacked vertically in a single QGraphicsScene,
    // so text stays selectable and shapes scale as vectors. Slides fit the viewport
    // width by default; Zoom In/Out multiply that fit via the view transform (no
    // re-rasterization -- crisp at any zoom).
    QWidget *m_slidesPage = nullptr;
    QGraphicsView *m_slidesView = nullptr;
    QGraphicsScene *m_slidesScene = nullptr;
    QLabel *m_slidesInfo = nullptr;              // "Slide N / M" per scroll position
    // Per-slide bookkeeping. Every slide's size + text is parsed up front (cheap),
    // but only on-screen slides get a full item tree built (buildSlidePage decodes
    // embedded images -- expensive); off-screen slides hold a lightweight white
    // placeholder rect so the scrollbar range, page numbers and positioning stay
    // correct without paying the build cost. m_slidePageItems[i] is whichever is
    // currently in the scene; m_slideBuilt[i] says which.
    QVector<QByteArray> m_slideSvgs;             // raw SVG per slide (for on-demand build)
    QVector<QGraphicsItem *> m_slidePageItems;   // current scene item per slide (placeholder or full)
    QVector<bool> m_slideBuilt;                  // whether the full item tree is built
    QVector<double> m_slidePageTop;              // scene-Y of each slide's page block
    QVector<QSizeF> m_slideSizes;                // scene size of each slide's page block
    QVector<QString> m_slideTexts;               // concatenated text per slide (copy fallback)
    double m_slidesSceneWidth = 0.0;             // widest page (scene units) for fit-to-width
    QTimer *m_slidesRelayoutTimer = nullptr;     // debounce viewport resizes before re-fitting
    double m_slidesZoom = 1.0;                    // user zoom multiplier on top of fit-to-width; 1.0 == fit
    // A freshly loaded deck must open at the top of slide 1. Until the initial
    // fit (plus its deferred re-fit) has settled, relayoutSlides forces the
    // scrollbar to 0 and ignores the "preserve scroll ratio" logic, which would
    // otherwise carry over the *previous* deck's scroll position on a file switch.
    bool m_slidesResetScroll = false;

    // Video page.
    QString m_videoPath;
    int m_videoRotation = 0; // quarter turns applied to the current clip            // path of the clip currently loaded (de-dup re-selects)
    QWidget *m_videoPage = nullptr;
    QWidget *m_videoSurface = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_muteButton = nullptr; // checkable: checked == muted
    QComboBox *m_speedCombo = nullptr;
    QSlider *m_progressSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QCheckBox *m_videoInfoCheck = nullptr;
    QLabel *m_videoInfoOverlay = nullptr;
    QLabel *m_videoNotice = nullptr;      // transient, self-hiding
    QTimer *m_videoNoticeTimer = nullptr;
    QTimer *m_videoTimer = nullptr; // polls position while playing
    bool m_seeking = false;         // suppress timer updates while dragging

    // Audio page: cover art + tags + lyrics + a transport row
    // (play/pause, prev/next track, seek slider with elapsed/total labels).
    QWidget *m_audioPage = nullptr;
    QLabel *m_audioCover = nullptr;   // embedded cover art or a placeholder glyph
    QPixmap m_audioCoverSource;       // the cover as decoded, before any tint
    QLabel *m_audioTitle = nullptr;   // big title line
    QLabel *m_audioMeta = nullptr;    // artist / album / year / genre / track
    QTextBrowser *m_audioLyrics = nullptr;
    QPushButton *m_audioPlayButton = nullptr;
    QPushButton *m_audioPrevButton = nullptr;
    QPushButton *m_audioNextButton = nullptr;
    QPushButton *m_audioMuteButton = nullptr; // checkable: checked == muted
    QSlider *m_audioSeek = nullptr;
    QSlider *m_audioVolumeSlider = nullptr;
    QLabel *m_audioElapsed = nullptr;
    QLabel *m_audioTotal = nullptr;
    QTimer *m_audioTimer = nullptr;  // polls position while playing
    bool m_audioSeeking = false;     // suppress timer updates while dragging
    QString m_audioPath;             // path of the track currently loaded
    QStringList m_audioSiblings;     // sibling audio files for prev/next
    int m_audioSiblingIndex = -1;

    MediaEngine *m_mediaEngine = nullptr; // one configured backend shared by audio and video
    std::unique_ptr<MediaEngine> m_pendingMediaEngine;
    bool m_mediaEngineReady = false;
    bool m_mediaEngineFailed = false;
    QString m_mediaEngineFailureMessage;
    Settings &m_settings; // persisted video speed / volume / mute
    Context m_context;    // Embedded (Ctrl+Q pane) vs Window (F3 viewer)
    QString m_contentFontFamily;
    int m_contentFontSize = -1;

    ImagePreviewLoader *m_imageLoader = nullptr;
    QImage m_originalImage;
    ImageMetadata m_imageMetadata;
    QTransform m_imageTransform;
    quint64 m_imageGeneration = 0;
    quint64 m_pendingImageLoadGeneration = 0;
    quint64 m_pendingImageRenderGeneration = 0;
    bool m_imageRevealPending = false;
    QString m_pendingImagePath;
    QHash<QString, int> m_pendingImageRotations;
    double m_imageScale = 1.0;
    bool m_imageFitMode = true; // re-fit on resize until the user zooms

    // Left-drag panning state.
    bool m_panning = false;
    QPoint m_panStart;
    int m_panHScroll = 0;
    int m_panVScroll = 0;
};
