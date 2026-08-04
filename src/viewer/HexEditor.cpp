#include "HexEditor.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringList>
#include <QUndoCommand>
#include <QUndoStack>

namespace {

// One buffer, held whole, so an offset is an index and nothing has to be
// re-read to answer a question about it. 256 MiB is where that stops being a
// good trade: see the measurements in the delivery report -- load and paint
// stay comfortable well past it, but an insert-mode keystroke is a memmove of
// everything after the caret, and that is what puts a floor under the
// interactive cost. Refusing loudly at a known number beats discovering the
// limit as a freeze on somebody's disk image.
constexpr qint64 kMaximumSize = 256LL * 1024 * 1024;

// Tinting edited bytes needs the loaded bytes kept alongside the edited ones,
// which doubles the memory a file costs and makes the first keystroke pay for
// detaching a shared buffer (measured at 89 ms on a 256 MiB file). Below this
// size both are noise; above it the tint is dropped rather than charged for.
constexpr qint64 kEditHighlightLimit = 64LL * 1024 * 1024;

// Consecutive edits to the same byte (the two nibbles of one hex byte) collapse
// into a single undo step; anything else is its own step.
constexpr int kSpliceMergeId = 0x48455845;

bool isPrintableAscii(char byte) {
    const uchar value = static_cast<uchar>(byte);
    return value >= 0x20 && value < 0x7f;
}

int hexValue(QChar character) {
    const char latin = character.toLatin1();
    if (latin >= '0' && latin <= '9')
        return latin - '0';
    if (latin >= 'a' && latin <= 'f')
        return latin - 'a' + 10;
    if (latin >= 'A' && latin <= 'F')
        return latin - 'A' + 10;
    return -1;
}

QString hexByte(char byte) {
    static const char digits[] = "0123456789ABCDEF";
    const uchar value = static_cast<uchar>(byte);
    QString text(2, QLatin1Char(' '));
    text[0] = QLatin1Char(digits[value >> 4]);
    text[1] = QLatin1Char(digits[value & 0x0f]);
    return text;
}

} // namespace

// Every mutation -- overwrite, insert, delete, and undo of each -- is the same
// operation: replace `removeCount` bytes at `position` with `inserted`. Having
// one shape means undo is just the inverse splice, and two adjacent splices can
// be merged by inspection instead of by special-casing edit kinds.
class HexEditor::SpliceCommand : public QUndoCommand {
public:
    SpliceCommand(HexEditor *editor, qint64 position, const QByteArray &removed,
                  const QByteArray &inserted, qint64 cursorBefore, qint64 cursorAfter,
                  bool mergeable)
        : m_editor(editor)
        , m_position(position)
        , m_removed(removed)
        , m_inserted(inserted)
        , m_cursorBefore(cursorBefore)
        , m_cursorAfter(cursorAfter)
        , m_mergeable(mergeable) {}

    void redo() override {
        m_editor->applySplice(m_position, m_removed.size(), m_inserted, m_cursorAfter);
    }

    void undo() override {
        m_editor->applySplice(m_position, m_inserted.size(), m_removed, m_cursorBefore);
    }

    int id() const override { return m_mergeable ? kSpliceMergeId : -1; }

    bool mergeWith(const QUndoCommand *other) override {
        const auto *next = static_cast<const SpliceCommand *>(other);
        if (!m_mergeable || !next->m_mergeable || next->m_removed.size() != 1 ||
            m_inserted.isEmpty())
            return false;
        // Only the byte this command last wrote may be overwritten in place --
        // that is the low nibble landing on the high nibble typed a moment ago.
        if (next->m_position != m_position + m_inserted.size() - 1 ||
            m_inserted.right(1) != next->m_removed)
            return false;
        m_inserted = m_inserted.left(m_inserted.size() - 1) + next->m_inserted;
        m_cursorAfter = next->m_cursorAfter;
        return true;
    }

private:
    HexEditor *m_editor;
    qint64 m_position;
    QByteArray m_removed;
    QByteArray m_inserted;
    qint64 m_cursorBefore;
    qint64 m_cursorAfter;
    bool m_mergeable;
};

HexEditor::HexEditor(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_undoStack(new QUndoStack(this)) {
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    // The FAMILY from the system's fixed-pitch font, the SIZE from whatever
    // this widget inherits. systemFont() carries its own point size (9 pt on
    // Windows) and setting it whole overrode the application font the user
    // configured, so the hex dump came out visibly smaller than the text editor
    // beside it -- and left the right of the window empty.
    setFont(fontFor(QFontDatabase::systemFont(QFontDatabase::FixedFont), font()));

    connect(m_undoStack, &QUndoStack::cleanChanged, this,
            [this](bool clean) { emit modificationChanged(!clean); });

    recomputeMetrics();
}

HexEditor::~HexEditor() {
    // QWidget::~QWidget deletes child QObjects while this object's connections
    // are still live but its own subclass part is already gone, and
    // ~QUndoStack clears the stack -- which emits cleanChanged() on the way
    // out. Relaying that would hand every observer a modificationChanged()
    // from a half-destroyed editor. Nobody wants to hear about modification
    // state during teardown, so the relay is cut here.
    m_undoStack->disconnect(this);
}

QFont HexEditor::fontFor(const QFont &systemFixed, const QFont &inherited) {
    QFont result = systemFixed;
    // Point size where there is one, pixel size otherwise: a font may carry
    // either, and taking only the point size would silently keep the system
    // font's own dimensions for a pixel-sized configuration.
    if (inherited.pointSizeF() > 0)
        result.setPointSizeF(inherited.pointSizeF());
    else if (inherited.pixelSize() > 0)
        result.setPixelSize(inherited.pixelSize());
    return result;
}

qint64 HexEditor::maximumSize() {
    return kMaximumSize;
}

bool HexEditor::fitsInEditor(qint64 sizeBytes) {
    return sizeBytes >= 0 && sizeBytes <= kMaximumSize;
}

QString HexEditor::oversizeMessage(qint64 sizeBytes) {
    return tr("This file is %1 and cannot be opened in the hex editor, which "
              "holds the whole file in memory and is limited to %2.")
        .arg(QLocale().formattedDataSize(sizeBytes),
             QLocale().formattedDataSize(kMaximumSize));
}

bool HexEditor::setContents(const QByteArray &data) {
    if (!fitsInEditor(data.size()))
        return false;

    m_data = data;
    m_original = data.size() <= kEditHighlightLimit ? data : QByteArray();
    m_undoStack->clear();
    m_cursor = 0;
    m_anchor = 0;
    m_lowNibble = false;
    recomputeMetrics();
    updateScrollBars();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    viewport()->update();
    emit contentsChanged();
    emit cursorPositionChanged(m_cursor);
    emit selectionChanged();
    return true;
}

QByteArray HexEditor::contents() const {
    return m_data;
}

qint64 HexEditor::size() const {
    return m_data.size();
}

bool HexEditor::isModified() const {
    return !m_undoStack->isClean();
}

void HexEditor::setModified(bool modified) {
    if (modified == isModified())
        return;
    if (modified) {
        m_undoStack->resetClean();
    } else {
        // The bytes on screen are the new baseline, so the edited-byte tint
        // clears with the modified flag.
        m_original = m_data.size() <= kEditHighlightLimit ? m_data : QByteArray();
        m_undoStack->setClean();
        viewport()->update();
    }
}

bool HexEditor::isOverwriteMode() const {
    return m_overwrite;
}

void HexEditor::setOverwriteMode(bool overwrite) {
    if (m_overwrite == overwrite)
        return;
    m_overwrite = overwrite;
    viewport()->update();
    emit overwriteModeChanged(m_overwrite);
}

bool HexEditor::isReadOnly() const {
    return m_readOnly;
}

void HexEditor::setReadOnly(bool readOnly) {
    m_readOnly = readOnly;
}

qint64 HexEditor::cursorPosition() const {
    return m_cursor;
}

void HexEditor::setCursorPosition(qint64 offset) {
    moveCursor(offset, false);
}

HexEditor::Column HexEditor::activeColumn() const {
    return m_column;
}

void HexEditor::setActiveColumn(Column column) {
    if (m_column == column)
        return;
    m_column = column;
    m_lowNibble = false;
    viewport()->update();
}

qint64 HexEditor::selectionStart() const {
    return qMin(m_anchor, m_cursor);
}

qint64 HexEditor::selectionLength() const {
    return qAbs(m_cursor - m_anchor);
}

QByteArray HexEditor::selectedBytes() const {
    return m_data.mid(static_cast<int>(selectionStart()), static_cast<int>(selectionLength()));
}

void HexEditor::selectRange(qint64 offset, qint64 length) {
    m_anchor = qBound<qint64>(0, offset, m_data.size());
    m_cursor = qBound<qint64>(0, offset + length, m_data.size());
    m_lowNibble = false;
    ensureCursorVisible();
    viewport()->update();
    emit cursorPositionChanged(m_cursor);
    emit selectionChanged();
}

int HexEditor::bytesPerLine() const {
    return m_bytesPerLine;
}

void HexEditor::setBytesPerLine(int count) {
    // An explicit choice wins permanently: the caller means this number, not
    // "start here and let the window change it".
    m_autoBytesPerLine = false;
    if (count <= 0 || count == m_bytesPerLine)
        return;
    m_bytesPerLine = count;
    m_groupSize = count >= 8 ? 8 : count;
    recomputeMetrics();
    updateScrollBars();
    viewport()->update();
}

QUndoStack *HexEditor::undoStack() const {
    return m_undoStack;
}

// --- theming ---------------------------------------------------------------

QColor HexEditor::resolved(const QColor &explicitColor, const QColor &fallback) const {
    return explicitColor.isValid() ? explicitColor : fallback;
}

// A fraction of `towards` mixed into `base`.
//
// The address column's fallback used to be QPalette::AlternateBase, and no
// theme in this app sets that role -- it stayed at the platform default and
// painted a white strip down the left of a dark window. Exactly the bug the
// text editor's line-number gutter had. Base and Text are roles the themes DO
// set, so deriving from them cannot come out white on a dark background even if
// a theme names no colour of its own.
QColor HexEditor::mixed(const QColor &base, const QColor &towards, double amount) {
    const double keep = 1.0 - amount;
    return QColor::fromRgbF(base.redF() * keep + towards.redF() * amount,
                            base.greenF() * keep + towards.greenF() * amount,
                            base.blueF() * keep + towards.blueF() * amount,
                            base.alphaF());
}

#define HEX_EDITOR_COLOR(Getter, Setter, Member)                                                  \
    QColor HexEditor::Getter() const { return Member; }                                           \
    void HexEditor::Setter(const QColor &color) {                                                 \
        Member = color;                                                                           \
        viewport()->update();                                                                     \
    }

HEX_EDITOR_COLOR(backgroundColor, setBackgroundColor, m_backgroundColor)
HEX_EDITOR_COLOR(textColor, setTextColor, m_textColor)
HEX_EDITOR_COLOR(addressColor, setAddressColor, m_addressColor)
HEX_EDITOR_COLOR(addressBackgroundColor, setAddressBackgroundColor, m_addressBackgroundColor)
HEX_EDITOR_COLOR(separatorColor, setSeparatorColor, m_separatorColor)
HEX_EDITOR_COLOR(selectionColor, setSelectionColor, m_selectionColor)
HEX_EDITOR_COLOR(selectionTextColor, setSelectionTextColor, m_selectionTextColor)
HEX_EDITOR_COLOR(cursorColor, setCursorColor, m_cursorColor)
HEX_EDITOR_COLOR(modifiedColor, setModifiedColor, m_modifiedColor)
HEX_EDITOR_COLOR(nonPrintableColor, setNonPrintableColor, m_nonPrintableColor)

#undef HEX_EDITOR_COLOR

// --- geometry --------------------------------------------------------------

void HexEditor::recomputeMetrics() {
    const QFontMetrics metrics(font());
    m_charWidth = qMax(1, metrics.horizontalAdvance(QLatin1Char('0')));
    m_rowHeight = metrics.height() + 2;
    m_ascent = metrics.ascent() + 1;
    m_margin = m_charWidth;

    qint64 highest = qMax<qint64>(m_data.size(), 1);
    int digits = 0;
    while (highest > 0) {
        ++digits;
        highest >>= 4;
    }
    m_addressChars = qMax(8, digits);
    m_addressWidth = m_addressChars * m_charWidth + 2 * m_margin;
    m_hexOriginX = m_addressWidth + m_margin;

    const int lastCell = m_bytesPerLine > 0 ? m_bytesPerLine - 1 : 0;
    const int hexChars = 3 * lastCell + lastCell / m_groupSize + 2;
    m_asciiOriginX = m_hexOriginX + hexChars * m_charWidth + 2 * m_charWidth;
}

int HexEditor::hexCellX(int index) const {
    return m_hexOriginX + (3 * index + index / m_groupSize) * m_charWidth;
}

int HexEditor::asciiCellX(int index) const {
    return m_asciiOriginX + index * m_charWidth;
}

int HexEditor::totalWidth() const {
    return m_asciiOriginX + m_bytesPerLine * m_charWidth + m_margin;
}

void HexEditor::updateScrollBars() {
    const int visibleRows = qMax(1, viewport()->height() / m_rowHeight);
    // One row past the last full one, so the caret can sit at end-of-file.
    const int rowCount = static_cast<int>(m_data.size() / m_bytesPerLine) + 1;

    verticalScrollBar()->setRange(0, qMax(0, rowCount - visibleRows));
    verticalScrollBar()->setPageStep(visibleRows);
    verticalScrollBar()->setSingleStep(1);

    horizontalScrollBar()->setRange(0, qMax(0, totalWidth() - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(m_charWidth);
}

void HexEditor::ensureCursorVisible() {
    const int row = static_cast<int>(m_cursor / m_bytesPerLine);
    const int visibleRows = qMax(1, viewport()->height() / m_rowHeight);
    if (row < verticalScrollBar()->value())
        verticalScrollBar()->setValue(row);
    else if (row >= verticalScrollBar()->value() + visibleRows)
        verticalScrollBar()->setValue(row - visibleRows + 1);
}

qint64 HexEditor::offsetAt(const QPoint &viewportPoint, Column *column) const {
    const int row = verticalScrollBar()->value() + viewportPoint.y() / m_rowHeight;
    const int x = viewportPoint.x() + horizontalScrollBar()->value();

    int index = 0;
    if (x >= m_asciiOriginX - m_charWidth) {
        if (column)
            *column = Column::Ascii;
        index = (x - m_asciiOriginX) / m_charWidth;
    } else {
        if (column)
            *column = Column::Hex;
        for (int i = 0; i < m_bytesPerLine; ++i) {
            index = i;
            if (x < hexCellX(i) + 3 * m_charWidth)
                break;
        }
    }
    index = qBound(0, index, m_bytesPerLine - 1);
    return qBound<qint64>(0, static_cast<qint64>(row) * m_bytesPerLine + index, m_data.size());
}

// --- painting --------------------------------------------------------------

void HexEditor::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());
    painter.setFont(font());

    const QPalette &colors = palette();
    const QColor background = resolved(m_backgroundColor, colors.color(QPalette::Base));
    const QColor text = resolved(m_textColor, colors.color(QPalette::Text));
    const QColor addressBackground =
        resolved(m_addressBackgroundColor,
                 mixed(colors.color(QPalette::Base), colors.color(QPalette::Text), 0.10));
    const QColor address = resolved(m_addressColor, colors.color(QPalette::Mid));
    const QColor separator = resolved(m_separatorColor, colors.color(QPalette::Mid));
    const QColor selection = resolved(m_selectionColor, colors.color(QPalette::Highlight));
    const QColor selectionText =
        resolved(m_selectionTextColor, colors.color(QPalette::HighlightedText));
    const QColor caret = resolved(m_cursorColor, colors.color(QPalette::Highlight));
    const QColor modified = resolved(m_modifiedColor, colors.color(QPalette::Link));
    const QColor nonPrintable = resolved(m_nonPrintableColor, colors.color(QPalette::Mid));

    painter.fillRect(event->rect(), background);

    const int dx = -horizontalScrollBar()->value();
    painter.fillRect(QRect(dx, 0, m_addressWidth, viewport()->height()), addressBackground);
    painter.setPen(separator);
    painter.drawLine(dx + m_addressWidth, 0, dx + m_addressWidth, viewport()->height());
    painter.drawLine(dx + m_asciiOriginX - m_charWidth, 0, dx + m_asciiOriginX - m_charWidth,
                     viewport()->height());

    const qint64 selectionFrom = selectionStart();
    const qint64 selectionTo = selectionFrom + selectionLength();
    // Tinting edited bytes needs a byte-for-byte counterpart in the loaded
    // snapshot; once an insert or delete has shifted things there is no such
    // correspondence -- and past kEditHighlightLimit no snapshot is kept at
    // all -- so the tint switches off rather than lying about it.
    const bool trackEdits = !m_original.isEmpty() && m_original.size() == m_data.size();

    const int firstRow = verticalScrollBar()->value();
    const int rowsOnScreen = viewport()->height() / m_rowHeight + 2;

    for (int screenRow = 0; screenRow < rowsOnScreen; ++screenRow) {
        const int row = firstRow + screenRow;
        const qint64 rowOffset = static_cast<qint64>(row) * m_bytesPerLine;
        if (rowOffset > m_data.size())
            break;
        const int y = screenRow * m_rowHeight;
        const int baseline = y + m_ascent;

        painter.setPen(address);
        painter.drawText(dx + m_margin, baseline,
                         QStringLiteral("%1").arg(rowOffset, m_addressChars, 16,
                                                  QLatin1Char('0')).toUpper());

        for (int i = 0; i < m_bytesPerLine; ++i) {
            const qint64 offset = rowOffset + i;
            if (offset >= m_data.size())
                break;
            const char byte = m_data.at(static_cast<int>(offset));
            const bool selected = offset >= selectionFrom && offset < selectionTo;
            const bool edited = trackEdits && m_original.at(static_cast<int>(offset)) != byte;

            const QRect hexCell(dx + hexCellX(i), y, 2 * m_charWidth, m_rowHeight);
            const QRect asciiCell(dx + asciiCellX(i), y, m_charWidth, m_rowHeight);
            if (selected) {
                painter.fillRect(hexCell, selection);
                painter.fillRect(asciiCell, selection);
            }

            painter.setPen(selected ? selectionText : (edited ? modified : text));
            painter.drawText(hexCell.left(), baseline, hexByte(byte));

            if (isPrintableAscii(byte)) {
                painter.drawText(asciiCell.left(), baseline, QString(QChar::fromLatin1(byte)));
            } else {
                painter.setPen(selected ? selectionText : nonPrintable);
                painter.drawText(asciiCell.left(), baseline, QStringLiteral("."));
            }
        }

        if (m_cursor >= rowOffset && m_cursor < rowOffset + m_bytesPerLine &&
            (m_cursor < m_data.size() || rowOffset + m_bytesPerLine > m_data.size())) {
            const int i = static_cast<int>(m_cursor - rowOffset);
            const bool hexActive = m_column == Column::Hex;
            const int cellX = dx + (hexActive ? hexCellX(i) : asciiCellX(i));
            const int cellWidth = hexActive ? 2 * m_charWidth : m_charWidth;
            painter.setPen(caret);
            if (m_overwrite) {
                const int nibbleWidth = hexActive ? m_charWidth : cellWidth;
                const int nibbleX = cellX + (hexActive && m_lowNibble ? m_charWidth : 0);
                painter.fillRect(QRect(nibbleX, y + m_rowHeight - 2, nibbleWidth, 2), caret);
            } else {
                painter.fillRect(QRect(cellX, y, 2, m_rowHeight), caret);
            }
            painter.drawRect(QRect(cellX, y, cellWidth - 1, m_rowHeight - 1));
        }
    }
}

void HexEditor::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    fitBytesPerLineToWidth();
    updateScrollBars();
}

// A hex dump is conventionally sixteen bytes wide, and in a window this size
// that left the right half of it empty. The row grows to whatever the window
// can show instead, in groups of eight -- so the grouping stays readable and an
// offset is still easy to work out by eye.
void HexEditor::fitBytesPerLineToWidth() {
    if (!m_autoBytesPerLine || m_charWidth <= 0)
        return;
    // Mirrors recomputeMetrics(): everything left of the hex block is fixed,
    // then each group of eight costs 8 hex cells (three characters each), one
    // character of group spacing, and 8 characters of ASCII.
    const int perGroup = (8 * 3 + 1 + 8) * m_charWidth;
    const int gap = 2 * m_charWidth; // between the hex block and the ASCII column
    const int available = viewport()->width() - m_hexOriginX - gap - m_margin;
    if (available <= 0)
        return;
    const int groups = qBound(1, available / qMax(1, perGroup), 8);
    const int wanted = groups * 8;
    if (wanted == m_bytesPerLine)
        return;
    m_bytesPerLine = wanted;
    m_groupSize = 8;
    recomputeMetrics();
}

void HexEditor::changeEvent(QEvent *event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::PaletteChange) {
        recomputeMetrics();
        updateScrollBars();
        viewport()->update();
    }
}

bool HexEditor::focusNextPrevChild(bool next) {
    // Tab belongs to the widget: it swaps the hex and ASCII columns.
    Q_UNUSED(next);
    return false;
}

// --- editing ---------------------------------------------------------------

void HexEditor::applySplice(qint64 position, qint64 removeCount, const QByteArray &inserted,
                            qint64 cursorAfter) {
    m_data.replace(static_cast<int>(position), static_cast<int>(removeCount), inserted);
    m_cursor = qBound<qint64>(0, cursorAfter, m_data.size());
    m_anchor = m_cursor;
    m_lowNibble = false;
    recomputeMetrics();
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    emit contentsChanged();
    emit cursorPositionChanged(m_cursor);
    emit selectionChanged();
}

void HexEditor::pushSplice(qint64 position, qint64 removeCount, const QByteArray &inserted,
                           qint64 cursorAfter, bool mergeable) {
    const QByteArray removed =
        m_data.mid(static_cast<int>(position), static_cast<int>(removeCount));
    m_undoStack->push(new SpliceCommand(this, position, removed, inserted, m_cursor, cursorAfter,
                                        mergeable));
}

bool HexEditor::replaceSelection(const QByteArray &replacement) {
    const qint64 length = selectionLength();
    if (length == 0)
        return false;
    const qint64 from = selectionStart();
    pushSplice(from, length, replacement, from + replacement.size(), false);
    return true;
}

void HexEditor::typeHexDigit(int value) {
    // Appending at end-of-file is always allowed: overwrite mode protects the
    // structure *inside* a file, and there is nothing after the last byte.
    const bool atEnd = m_cursor >= m_data.size();
    const bool growsFile = atEnd || (!m_overwrite && !m_lowNibble);
    if (growsFile && m_data.size() >= maximumSize())
        return;
    if (growsFile) {
        pushSplice(m_cursor, 0, QByteArray(1, static_cast<char>(value << 4)), m_cursor, true);
        m_lowNibble = true;
        viewport()->update();
        return;
    }

    const char current = m_data.at(static_cast<int>(m_cursor));
    const char updated =
        m_lowNibble ? static_cast<char>((current & 0xf0) | value)
                    : static_cast<char>((value << 4) | (current & 0x0f));
    const qint64 cursorAfter = m_lowNibble ? m_cursor + 1 : m_cursor;
    const bool wasLowNibble = m_lowNibble;
    pushSplice(m_cursor, 1, QByteArray(1, updated), cursorAfter, true);
    m_lowNibble = !wasLowNibble;
    viewport()->update();
}

void HexEditor::typeByte(char byte) {
    if (m_data.size() >= maximumSize() && (m_cursor >= m_data.size() || !m_overwrite))
        return;
    if (m_overwrite && m_cursor < m_data.size())
        pushSplice(m_cursor, 1, QByteArray(1, byte), m_cursor + 1, false);
    else
        pushSplice(m_cursor, 0, QByteArray(1, byte), m_cursor + 1, false);
}

void HexEditor::removeSelectionOrByte(bool backwards) {
    if (selectionLength() > 0) {
        if (m_overwrite) {
            // Deleting bytes would change the file size, which is precisely
            // what overwrite mode exists to prevent, so a selection is zeroed
            // in place instead.
            const qint64 length = selectionLength();
            replaceSelection(QByteArray(static_cast<int>(length), '\0'));
        } else {
            replaceSelection(QByteArray());
        }
        return;
    }
    if (m_overwrite)
        return;
    if (backwards) {
        if (m_cursor <= 0)
            return;
        pushSplice(m_cursor - 1, 1, QByteArray(), m_cursor - 1, false);
    } else {
        if (m_cursor >= m_data.size())
            return;
        pushSplice(m_cursor, 1, QByteArray(), m_cursor, false);
    }
}

void HexEditor::moveCursor(qint64 offset, bool extendSelection) {
    const qint64 clamped = qBound<qint64>(0, offset, m_data.size());
    m_cursor = clamped;
    if (!extendSelection)
        m_anchor = clamped;
    m_lowNibble = false;
    ensureCursorVisible();
    viewport()->update();
    emit cursorPositionChanged(m_cursor);
    emit selectionChanged();
}

void HexEditor::keyPressEvent(QKeyEvent *event) {
    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
    const int visibleRows = qMax(1, viewport()->height() / m_rowHeight);

    if (event->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        redo();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        copy();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
        moveCursor(m_cursor - 1, extend);
        return;
    case Qt::Key_Right:
        moveCursor(m_cursor + 1, extend);
        return;
    case Qt::Key_Up:
        moveCursor(m_cursor - m_bytesPerLine, extend);
        return;
    case Qt::Key_Down:
        moveCursor(m_cursor + m_bytesPerLine, extend);
        return;
    case Qt::Key_PageUp:
        moveCursor(m_cursor - static_cast<qint64>(visibleRows) * m_bytesPerLine, extend);
        return;
    case Qt::Key_PageDown:
        moveCursor(m_cursor + static_cast<qint64>(visibleRows) * m_bytesPerLine, extend);
        return;
    case Qt::Key_Home:
        moveCursor(event->modifiers().testFlag(Qt::ControlModifier)
                       ? 0
                       : m_cursor - m_cursor % m_bytesPerLine,
                   extend);
        return;
    case Qt::Key_End:
        moveCursor(event->modifiers().testFlag(Qt::ControlModifier)
                       ? m_data.size()
                       : qMin<qint64>(m_data.size(),
                                      m_cursor - m_cursor % m_bytesPerLine + m_bytesPerLine - 1),
                   extend);
        return;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        setActiveColumn(m_column == Column::Hex ? Column::Ascii : Column::Hex);
        return;
    case Qt::Key_Insert:
        setOverwriteMode(!m_overwrite);
        return;
    case Qt::Key_Backspace:
        if (!m_readOnly)
            removeSelectionOrByte(true);
        return;
    case Qt::Key_Delete:
        if (!m_readOnly)
            removeSelectionOrByte(false);
        return;
    default:
        break;
    }

    if (m_readOnly || event->text().isEmpty() ||
        event->modifiers().testFlag(Qt::ControlModifier) ||
        event->modifiers().testFlag(Qt::AltModifier)) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    const QChar typed = event->text().at(0);
    if (m_column == Column::Hex) {
        const int digit = hexValue(typed);
        if (digit >= 0)
            typeHexDigit(digit);
        return;
    }
    // The ASCII column edits one byte per keystroke, so only characters that
    // survive a round trip through Latin-1 are accepted; anything wider would
    // silently become a different byte than the one on screen.
    const ushort code = typed.unicode();
    if (code >= 0x20 && code <= 0xff && code != 0x7f)
        typeByte(static_cast<char>(code));
}

void HexEditor::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    Column column = m_column;
    const qint64 offset = offsetAt(event->pos(), &column);
    setActiveColumn(column);
    m_selecting = true;
    moveCursor(offset, event->modifiers().testFlag(Qt::ShiftModifier));
}

void HexEditor::mouseMoveEvent(QMouseEvent *event) {
    if (!m_selecting) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    moveCursor(offsetAt(event->pos(), nullptr), true);
}

void HexEditor::mouseReleaseEvent(QMouseEvent *event) {
    m_selecting = false;
    QAbstractScrollArea::mouseReleaseEvent(event);
}

// --- slots -----------------------------------------------------------------

void HexEditor::undo() {
    if (m_undoStack->canUndo())
        m_undoStack->undo();
}

void HexEditor::redo() {
    if (m_undoStack->canRedo())
        m_undoStack->redo();
}

void HexEditor::copy() {
    const QByteArray bytes = selectedBytes();
    if (bytes.isEmpty())
        return;
    QString text;
    if (m_column == Column::Hex) {
        QStringList cells;
        cells.reserve(bytes.size());
        for (char byte : bytes)
            cells.append(hexByte(byte));
        text = cells.join(QLatin1Char(' '));
    } else {
        text = QString::fromLatin1(bytes);
    }
    QApplication::clipboard()->setText(text);
}

void HexEditor::selectAll() {
    selectRange(0, m_data.size());
}
