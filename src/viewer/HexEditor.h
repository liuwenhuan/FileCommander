#pragma once

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QColor>

class QUndoStack;

// Byte editor for everything F4 cannot sensibly open as text.
//
// Three columns -- offset, hex, ASCII -- with the caret editable in either of
// the last two. Overwrite is the default because a binary file's structure is
// usually offset-dependent: inserting one byte into an ELF header or a PNG
// chunk breaks every length and CRC after it, so growing the file has to be a
// deliberate act (the Insert key).
//
// It never touches the disk. contents() hands the bytes back; writing them is
// the caller's job, so nothing lands on disk until Save is pressed.
//
// Only the visible rows are laid out and painted, so scrolling cost does not
// grow with file size; the whole file still lives in one QByteArray, which is
// what maximumSize() bounds. Feed it setContents() only after asking
// fitsInEditor() -- see oversizeMessage() for the refusal to show the user.
class HexEditor : public QAbstractScrollArea {
    Q_OBJECT

    // Theming hooks. Every one of these defaults to an invalid QColor, which
    // means "derive it from the palette", so the widget is already correct in
    // all three themes without a single .qss rule. A theme that wants a
    // different accent overrides it as `HexEditor { qproperty-<name>: #rgb; }`.
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)
    Q_PROPERTY(QColor addressColor READ addressColor WRITE setAddressColor)
    Q_PROPERTY(QColor addressBackgroundColor READ addressBackgroundColor
                   WRITE setAddressBackgroundColor)
    Q_PROPERTY(QColor separatorColor READ separatorColor WRITE setSeparatorColor)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor)
    Q_PROPERTY(QColor selectionTextColor READ selectionTextColor WRITE setSelectionTextColor)
    Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor)
    Q_PROPERTY(QColor modifiedColor READ modifiedColor WRITE setModifiedColor)
    Q_PROPERTY(QColor nonPrintableColor READ nonPrintableColor WRITE setNonPrintableColor)

public:
    enum class Column { Hex, Ascii };

    explicit HexEditor(QWidget *parent = nullptr);
    ~HexEditor() override;

    // Largest file this widget accepts. See the class comment for why the
    // ceiling exists at all and docs in the report for how it was measured.
    static qint64 maximumSize();
    static bool fitsInEditor(qint64 sizeBytes);
    // User-visible refusal, already translated. Ask before reading the file.
    static QString oversizeMessage(qint64 sizeBytes);

    // Returns false and changes nothing when the data exceeds maximumSize().
    bool setContents(const QByteArray &data);
    QByteArray contents() const;
    qint64 size() const;

    // True while contents() differs from what setContents() was last given.
    // Undoing back to the loaded state clears it again.
    bool isModified() const;
    // Save calls setModified(false) to mark the current bytes as the new
    // baseline; it does not alter the undo history.
    void setModified(bool modified);

    bool isOverwriteMode() const;
    void setOverwriteMode(bool overwrite);

    bool isReadOnly() const;
    void setReadOnly(bool readOnly);

    qint64 cursorPosition() const;
    void setCursorPosition(qint64 offset);
    Column activeColumn() const;
    void setActiveColumn(Column column);

    // Selection is a byte range; length 0 means "just a caret".
    qint64 selectionStart() const;
    qint64 selectionLength() const;
    QByteArray selectedBytes() const;
    void selectRange(qint64 offset, qint64 length);

    int bytesPerLine() const;
    void setBytesPerLine(int count);

    // Exposed so a toolbar can drive QUndoStack::createUndoAction() rather
    // than duplicating enabled-state bookkeeping.
    QUndoStack *undoStack() const;

    QColor backgroundColor() const;
    void setBackgroundColor(const QColor &color);
    QColor textColor() const;
    void setTextColor(const QColor &color);
    QColor addressColor() const;
    void setAddressColor(const QColor &color);
    QColor addressBackgroundColor() const;
    void setAddressBackgroundColor(const QColor &color);
    QColor separatorColor() const;
    void setSeparatorColor(const QColor &color);
    QColor selectionColor() const;
    void setSelectionColor(const QColor &color);
    QColor selectionTextColor() const;
    void setSelectionTextColor(const QColor &color);
    QColor cursorColor() const;
    void setCursorColor(const QColor &color);
    QColor modifiedColor() const;
    void setModifiedColor(const QColor &color);
    QColor nonPrintableColor() const;
    void setNonPrintableColor(const QColor &color);

public slots:
    void undo();
    void redo();
    void copy();
    void selectAll();

signals:
    void contentsChanged();
    void modificationChanged(bool modified);
    void cursorPositionChanged(qint64 offset);
    void overwriteModeChanged(bool overwrite);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    class SpliceCommand;
    friend class SpliceCommand;

    void recomputeMetrics();
    void updateScrollBars();
    void ensureCursorVisible();
    void moveCursor(qint64 offset, bool extendSelection);
    // The single mutation point: every edit is one splice, so undo/redo and
    // the modified-byte highlight have exactly one path to keep in step.
    void applySplice(qint64 position, qint64 removeCount, const QByteArray &inserted,
                     qint64 cursorAfter);
    void pushSplice(qint64 position, qint64 removeCount, const QByteArray &inserted,
                    qint64 cursorAfter, bool mergeable);
    void typeHexDigit(int value);
    void typeByte(char byte);
    void removeSelectionOrByte(bool backwards);
    bool replaceSelection(const QByteArray &replacement);

    int hexCellX(int index) const;
    int asciiCellX(int index) const;
    int totalWidth() const;
    qint64 offsetAt(const QPoint &viewportPoint, Column *column) const;
    QColor resolved(const QColor &override, const QColor &fallback) const;

    QByteArray m_data;
    // Snapshot of the loaded bytes, kept only so edited bytes can be tinted.
    // Implicitly shared with m_data until the first edit, and left empty
    // entirely for files large enough for the second copy to matter.
    QByteArray m_original;
    QUndoStack *m_undoStack = nullptr;

    qint64 m_cursor = 0;
    qint64 m_anchor = 0;
    bool m_lowNibble = false;
    Column m_column = Column::Hex;
    bool m_overwrite = true;
    bool m_readOnly = false;
    bool m_selecting = false;

    int m_bytesPerLine = 16;
    int m_groupSize = 8;
    int m_charWidth = 8;
    int m_rowHeight = 16;
    int m_ascent = 12;
    int m_addressChars = 8;
    int m_addressWidth = 80;
    int m_hexOriginX = 80;
    int m_asciiOriginX = 400;
    int m_margin = 6;

    QColor m_backgroundColor;
    QColor m_textColor;
    QColor m_addressColor;
    QColor m_addressBackgroundColor;
    QColor m_separatorColor;
    QColor m_selectionColor;
    QColor m_selectionTextColor;
    QColor m_cursorColor;
    QColor m_modifiedColor;
    QColor m_nonPrintableColor;
};
