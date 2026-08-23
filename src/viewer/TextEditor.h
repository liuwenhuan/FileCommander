#pragma once

#include <QByteArray>
#include <QColor>
#include <QPlainTextEdit>

#include "FramelessWindow.h"

#include "text/TextEncodingDetector.h"
#include "ByteSearch.h"

class LineNumberArea;
class QComboBox;
class QLabel;
class QStackedWidget;
class QToolBar;
class QTimer;
class QVBoxLayout;
class QAction;
class Settings;

// Editable QPlainTextEdit with a line-number gutter.
//
// The gutter is a hand-painted child widget, so a stylesheet's
// `QPlainTextEdit { background-color: ... }` never reaches it: that rule sets
// this widget's Base/Text palette roles, and nothing at all on the gutter. The
// original code filled the gutter with the APPLICATION palette's AlternateBase,
// which no theme sheet ever writes -- so in every theme it stayed the platform
// default (near-white), which only looked wrong once a theme with a dark or
// coloured editor arrived.
//
// The three qproperty hooks below are how a theme names those colours outright,
// the same mechanism PlainHeaderView uses in FileListView.cpp:
//
//     CodeEditor { qproperty-gutterBackground: #0a1a0d; ... }
//
// Each defaults to an invalid QColor meaning "not themed"; the painter then
// derives a colour by mixing THIS widget's own Base and Text roles, which a
// stylesheet does set. That fallback is what keeps a sheet that forgot the
// hooks (or no sheet at all) from going back to a white strip on a dark editor.
//
// Selectors must name `CodeEditor`, not `QPlainTextEdit`: Qt applies a
// qproperty- rule to every widget the selector matches and warns on stderr for
// each one lacking the property, so a base-class selector would produce a
// warning per ordinary plain-text edit in the app on every polish.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
    Q_PROPERTY(QColor gutterBackground READ gutterBackground WRITE setGutterBackground)
    Q_PROPERTY(QColor gutterForeground READ gutterForeground WRITE setGutterForeground)
    Q_PROPERTY(QColor gutterBorder READ gutterBorder WRITE setGutterBorder)

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent *event);
    int lineNumberAreaWidth() const;

    // The gutter widget itself, so a test can grab its painted pixels.
    QWidget *lineNumberArea() const;

    QColor gutterBackground() const { return m_gutterBackground; }
    QColor gutterForeground() const { return m_gutterForeground; }
    QColor gutterBorder() const { return m_gutterBorder; }
    void setGutterBackground(const QColor &color);
    void setGutterForeground(const QColor &color);
    void setGutterBorder(const QColor &color);

    // The colours actually used by the painter: the themed value when a sheet
    // set one, otherwise the derived fallback. Exposed so a test can compare
    // them against the pixels that came out.
    QColor effectiveGutterBackground() const;
    QColor effectiveGutterForeground() const;
    QColor effectiveGutterBorder() const;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);

private:
    LineNumberArea *m_lineNumberArea;
    QColor m_gutterBackground;
    QColor m_gutterForeground;
    QColor m_gutterBorder;
};

// Editable text file viewer, wired to F4.
//
// m_raw holds the loaded portion of the file. Large files start with a
// bounded prefix; Save re-reads and streams the untouched tail so that prefix
// edits never discard the rest of the file.
//
// Edits live in the buffer and are written after a short idle debounce. Save /
// Ctrl+S remains an immediate flush, and a failed write leaves the buffer dirty
// so Preview or close cannot silently discard it.
//
// FramelessWindow, not a bare QWidget: this window kept the native decorations
// (a stock light-grey title bar over a themed body) because the sweep that made
// the app's chrome themable converted the QDialogs and these two QWidget
// windows were not in that set.
class HexEditor;
class FindBar;

class TextEditor : public FramelessWindow {
    Q_OBJECT

public:
    explicit TextEditor(QWidget *parent = nullptr);
    TextEditor(Settings &settings, QWidget *parent = nullptr);

    bool loadFile(const QString &path);
    bool loadFile(const QString &path, const QString &encodingIdentity);

    // ---------------------------------------------------------------------
    // Extension seam
    //
    // Two features are planned for this window by other work and are given a
    // place to attach rather than a rewrite:
    //
    //   * A find bar -- a full-width row that appears BELOW the toolbar and
    //     ABOVE the view. Call addAuxiliaryBar(bar); the bar is stacked in
    //     order, starts hidden or shown as the caller left it, and never
    //     disturbs the toolbar or the view. Search against codeEditor().
    //
    //   * A hex view -- an alternative rendering of the same file. Call
    //     addView(widget) to add it as a page of viewStack() (the CodeEditor is
    //     page 0) and setCurrentView(index) to switch. Feed it fileBytes(),
    //     which is the file's on-disk content already in memory, so the hex
    //     view neither re-reads the file nor sees a decoded-and-re-encoded
    //     approximation of it. Add the toggle to toolBar().
    //
    // Both hooks are deliberately dumb: this class does not know what a find
    // bar or a hex view is, and does not manage their state.
    // ---------------------------------------------------------------------
    QToolBar *toolBar() const { return m_toolBar; }
    // Runs a search over the file's bytes and moves the active view to the hit.
    // Public so a test can drive it without synthesising key events.
    void runSearch(const ByteSearch::Needle &needle, ByteSearch::Direction direction);
    CodeEditor *codeEditor() const { return m_editor; }
    QStackedWidget *viewStack() const { return m_stack; }
    void addAuxiliaryBar(QWidget *bar);

    // Which of the two the file was opened as. A file that is not text is
    // edited as hex rather than being decoded into a buffer that could not
    // survive a round trip -- see fc::shouldEditAsHex.
    bool isHexMode() const { return m_hexMode; }
    HexEditor *hexEditor() const { return m_hex; }
    FindBar *findBar() const { return m_findBar; }
    // Opens the find bar and puts the caret in it. Bound to Ctrl+F and F3.
    void showFindBar();
    // Modified in EITHER view; see the note on the implementation.
    bool isDocumentModified() const;
    int addView(QWidget *view);
    void setCurrentView(int index);
    // The bytes currently materialised in the editor (a prefix for a lazily
    // loaded file).
    const QByteArray &fileBytes() const { return m_raw; }
    QString filePath() const { return m_path; }
    QString encodingIdentity() const { return m_encodingIdentity; }

    // Flushes a pending/dirty autosave synchronously. Public because an
    // embedded editor never gets a close event of its own: the host window has
    // to require a successful write before it leaves the editor page.
    bool promptSaveIfModified();

    // Test/seam accessors for the toolbar's own controls.
    QAction *saveAction() const { return m_saveAction; }
    QComboBox *encodingCombo() const { return m_encodingCombo; }
    // What the combo currently shows -- for Auto that includes the encoding the
    // detector settled on, e.g. "Auto (GB18030)".
    QString encodingStatusText() const;
    // The codec the buffer was decoded with and Save will encode with.
    QByteArray currentCodecName() const;

signals:
    // The Preview button was pressed. The host opens its preview window: this
    // library is below the UI layer and cannot reach it.
    void previewRequested(const QString &path);

public slots:
    // Returns false if nothing reached disk.
    bool save();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onModificationChanged(bool modified);
    void onContentChanged();
    void onEncodingSelected(int index);

private:
    TextEditor(Settings *settings, QWidget *parent);
    void updateTitle();
    void updateModifiedIndicator();
    // Re-decodes m_raw under the combo's current selection and replaces the
    // buffer. Never reads the buffer back -- the loaded bytes are the source
    // of truth for its visible portion.
    void applyEncodingToBuffer();
    void loadRemainder();
    void updatePartialIndicator();
    QByteArray encodeBuffer() const;
    bool writePartialBuffer(const QByteArray &prefix);
    bool writeCurrentBuffer();
    bool flushAutoSave();
    void showSaveFailure();
    void resetAutoSaveState();
    void buildToolBar();

    CodeEditor *m_editor = nullptr;
    QToolBar *m_toolBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QAction *m_saveAction = nullptr;
    QComboBox *m_encodingCombo = nullptr;
    QAction *m_loadRemainderAction = nullptr;
    QAction *m_previewAction = nullptr;
    QLabel *m_modifiedLabel = nullptr;
    QLabel *m_partialLabel = nullptr;
    QTimer *m_autoSaveTimer = nullptr;

    Settings *m_settings = nullptr;
    QString m_path;
    QString m_encodingIdentity;
    // The whole file, or the safe prefix for a large file. The tail begins at
    // m_loadedBytes on disk.
    QByteArray m_raw;
    qint64 m_loadedBytes = 0;
    bool m_partiallyLoaded = false;
    HexEditor *m_hex = nullptr;   // created only for a file opened as hex
    FindBar *m_findBar = nullptr;
    bool m_hexMode = false;
    bool m_installingContent = false;
    bool m_autoSaveBlocked = false;
    // Where the last hit was, so "find again" steps off it instead of
    // returning the same match forever.
    int m_lastMatchOffset = -1;
    TextEncodingDetector::Result m_detected;
    // QTextDocument cannot hold a CR at all -- it drops one on the way in --
    // so a CRLF file would come back out LF-only and every line of it would
    // read as changed to whatever diffs it next. Remembered at decode time and
    // put back at encode time instead.
    bool m_usesCrlf = false;
    // Index the combo was on when the buffer was last decoded. Used to put the
    // combo back when the user cancels an encoding change.
    int m_appliedEncodingIndex = 0;
    // Where the auxiliary bars (find bar) are inserted, and where the view
    // stack sits, in m_layout.
    int m_auxiliaryInsertIndex = 1;

    static constexpr int kAutoSaveDelayMs = 600;
    static constexpr qint64 kLazyLoadBytes = 5 * 1024 * 1024;
    static constexpr qint64 kTextReadLookAheadBytes = 4;
    static constexpr qint64 kTailCopyBytes = 64 * 1024;
};
