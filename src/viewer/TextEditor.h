#pragma once

#include <QPlainTextEdit>

#include "FramelessWindow.h"

class LineNumberArea;

// Editable QPlainTextEdit with a line-number gutter. Kept private to
// TextEditor -- callers only interact with TextEditor itself.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent *event);
    int lineNumberAreaWidth() const;

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);

private:
    LineNumberArea *m_lineNumberArea;
};

// Editable text file viewer, wired to F4. Tracks modification state and
// prompts to save on close.
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

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void save();
    void onModificationChanged(bool modified);

private:
    // Returns false if the user chose Cancel.
    bool promptSaveIfModified();
    void updateTitle();

    CodeEditor *m_editor;
    QString m_path;
    static constexpr qint64 kMaxEditableBytes = 50 * 1024 * 1024; // 50 MB
};
