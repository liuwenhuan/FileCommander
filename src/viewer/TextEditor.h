#pragma once

#include <QByteArray>
#include <QColor>
#include <QPlainTextEdit>

#include "FramelessWindow.h"

#include "text/TextEncodingDetector.h"

class LineNumberArea;
class QComboBox;
class QLabel;
class QStackedWidget;
class QToolBar;
class QVBoxLayout;
class QAction;

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
// The file's bytes as they exist on disk are kept in m_raw for the lifetime of
// the window. Everything else -- the decoded buffer, the encoding combo, Save
// -- is derived from them, which is what lets the encoding be changed after the
// fact without the original bytes having been thrown away.
//
// Edits live in the buffer only. Nothing is written until save() runs, and
// closing with unsaved changes still prompts.
//
// FramelessWindow, not a bare QWidget: this window kept the native decorations
// (a stock light-grey title bar over a themed body) because the sweep that made
// the app's chrome themable converted the QDialogs and these two QWidget
// windows were not in that set.
class TextEditor : public FramelessWindow {
    Q_OBJECT

public:
    explicit TextEditor(QWidget *parent = nullptr);

    bool loadFile(const QString &path);

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
    CodeEditor *codeEditor() const { return m_editor; }
    QStackedWidget *viewStack() const { return m_stack; }
    void addAuxiliaryBar(QWidget *bar);
    int addView(QWidget *view);
    void setCurrentView(int index);
    const QByteArray &fileBytes() const { return m_raw; }

    // Test/seam accessors for the toolbar's own controls.
    QAction *saveAction() const { return m_saveAction; }
    QComboBox *encodingCombo() const { return m_encodingCombo; }
    QString encodingStatusText() const;
    // The codec the buffer was decoded with and Save will encode with.
    QByteArray currentCodecName() const;

public slots:
    // Returns false if nothing reached disk.
    bool save();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onModificationChanged(bool modified);
    void onEncodingSelected(int index);

private:
    // Returns false if the user chose Cancel.
    bool promptSaveIfModified();
    void updateTitle();
    void updateModifiedIndicator();
    // Re-decodes m_raw under the combo's current selection and replaces the
    // buffer. Never reads the buffer back -- the bytes are the source of truth.
    void applyEncodingToBuffer();
    QByteArray encodeBuffer() const;
    void buildToolBar();

    CodeEditor *m_editor = nullptr;
    QToolBar *m_toolBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QAction *m_saveAction = nullptr;
    QComboBox *m_encodingCombo = nullptr;
    QLabel *m_encodingStatus = nullptr;
    QLabel *m_modifiedLabel = nullptr;

    QString m_path;
    // The file exactly as it is on disk. Refreshed by save(), so a later
    // encoding change always re-decodes what is really there.
    QByteArray m_raw;
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

    static constexpr qint64 kMaxEditableBytes = 50 * 1024 * 1024; // 50 MB
};
