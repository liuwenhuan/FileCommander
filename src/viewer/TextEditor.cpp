#include "TextEditor.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextCodec>
#include <QToolBar>

#include "ThemedDialogs.h"
#include <QPainter>
#include <QShortcut>
#include <QTextBlock>
#include <QVBoxLayout>

namespace {

// Encodings the user can force on the open file. Index 0 is Auto, which defers
// to TextEncodingDetector; every other entry is a deliberate override.
//
// Deliberately the same set, in the same order, as QuickView's preview toolbar
// (src/ui/QuickView.cpp): the two windows show the same files, and a list that
// diverged would make the preview and the editor disagree about what can even
// be selected. They are separate tables only because QuickView's is private to
// a src/ui translation unit; the detector they both defer to is shared.
struct TextEncoding {
    const char *label;
    const char *codec; // null: Auto (index 0) / the system locale codec
};
const TextEncoding kTextEncodings[] = {
    {"Auto", nullptr},
    {"UTF-8", "UTF-8"},
    {"UTF-16", "UTF-16"},
    {"ISO-8859-1", "ISO-8859-1"},
    {"GB18030", "GB18030"},
    {"Big5", "Big5"},
    {"Shift-JIS", "Shift-JIS"},
    {"EUC-JP", "EUC-JP"},
    {"EUC-KR", "EUC-KR"},
    {"Windows-1252", "Windows-1252"},
    {"System", nullptr},
};
constexpr int kAutoEncodingIndex = 0;
constexpr int kEncodingCount = int(sizeof(kTextEncodings) / sizeof(kTextEncodings[0]));

bool isPureAscii(const QByteArray &data) {
    for (char byte : data) {
        if (static_cast<unsigned char>(byte) >= 0x80)
            return false;
    }
    return !data.isEmpty();
}

// TextEncodingDetector's answer, with one guess overruled.
//
// The detector serves a READ-ONLY preview, where an exotic guess costs a glance
// and nothing else. Here it decides what gets WRITTEN BACK, so it gets one
// correction: pure-ASCII bytes of even length pair up into perfectly valid
// UTF-16 code units and can outscore the ASCII fallback, which would show a
// short ASCII file (a Makefile, a .desktop entry, a config snippet) as CJK
// mojibake AND then save it as UTF-16. ASCII is never a guess -- every byte is
// below 0x80 -- so where it is available it wins.
//
// Deliberately here and not in the detector: the detector is shared with
// QuickView and has a test suite pinning its scoring, and "prefer the reading
// that cannot be wrong" is a rule that belongs to writing files, not to
// classifying them.
TextEncodingDetector::Result detectForEditing(const QByteArray &raw) {
    TextEncodingDetector::Result result = TextEncodingDetector::detect(raw);
    if (!result.binary && result.bomBytes == 0 && isPureAscii(raw) &&
        result.codecName != QByteArrayLiteral("UTF-8")) {
        result.label = QStringLiteral("ASCII");
        result.codecName = QByteArrayLiteral("UTF-8");
        result.ambiguous = false;
        result.incompleteTail = false;
        result.completePrefixBytes = raw.size();
    }
    return result;
}

QColor mixColors(const QColor &from, const QColor &to, double amount) {
    const double keep = 1.0 - amount;
    return QColor(qRound(from.red() * keep + to.red() * amount),
                  qRound(from.green() * keep + to.green() * amount),
                  qRound(from.blue() * keep + to.blue() * amount));
}

} // namespace

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *editor) : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override { return QSize(m_editor->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->lineNumberAreaPaintEvent(event); }

private:
    CodeEditor *m_editor;
};

CodeEditor::CodeEditor(QWidget *parent) : QPlainTextEdit(parent) {
    m_lineNumberArea = new LineNumberArea(this);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont font(QStringLiteral("monospace"));
    setFont(font);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);

    updateLineNumberAreaWidth(0);
}

QWidget *CodeEditor::lineNumberArea() const {
    return m_lineNumberArea;
}

void CodeEditor::setGutterBackground(const QColor &color) {
    m_gutterBackground = color;
    m_lineNumberArea->update();
}

void CodeEditor::setGutterForeground(const QColor &color) {
    m_gutterForeground = color;
    m_lineNumberArea->update();
}

void CodeEditor::setGutterBorder(const QColor &color) {
    m_gutterBorder = color;
    m_lineNumberArea->update();
}

QColor CodeEditor::effectiveGutterBackground() const {
    if (m_gutterBackground.isValid())
        return m_gutterBackground;
    // Derived from THIS widget's Base/Text, which a stylesheet does write (the
    // application palette's AlternateBase, which the old code used, it does
    // not). Mixing rather than lighter()/darker() so it works from pure black
    // and pure white alike, and so it can never land far from the editor's own
    // background regardless of the theme's hue.
    return mixColors(palette().color(QPalette::Base), palette().color(QPalette::Text), 0.10);
}

QColor CodeEditor::effectiveGutterForeground() const {
    if (m_gutterForeground.isValid())
        return m_gutterForeground;
    return mixColors(palette().color(QPalette::Base), palette().color(QPalette::Text), 0.55);
}

QColor CodeEditor::effectiveGutterBorder() const {
    if (m_gutterBorder.isValid())
        return m_gutterBorder;
    return mixColors(palette().color(QPalette::Base), palette().color(QPalette::Text), 0.25);
}

void CodeEditor::changeEvent(QEvent *event) {
    QPlainTextEdit::changeEvent(event);
    // A stylesheet change re-polishes the widget and can rewrite both the
    // palette the fallback reads and the qproperty values themselves, so the
    // hand-painted gutter has to be told to repaint -- nothing else repaints it.
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::FontChange)
        m_lineNumberArea->update();
}

int CodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    int maxLine = qMax(1, blockCount());
    while (maxLine >= 10) {
        maxLine /= 10;
        ++digits;
    }
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy) {
    if (dy != 0)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event) {
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), effectiveGutterBackground());

    // A one-pixel rule on the inner edge, so the gutter reads as a column and
    // not as a smudge, in a theme whose gutter and editor are near the same
    // lightness.
    const int rule = m_lineNumberArea->width() - 1;
    if (rule >= event->rect().left() && rule <= event->rect().right()) {
        painter.setPen(effectiveGutterBorder());
        painter.drawLine(rule, event->rect().top(), rule, event->rect().bottom());
    }

    const QColor numberColor = effectiveGutterForeground();
    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(numberColor);
            painter.drawText(0, top, m_lineNumberArea->width() - 6, fontMetrics().height(),
                              Qt::AlignRight, number);
        }
        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

TextEditor::TextEditor(QWidget *parent) : FramelessWindow(parent) {
    setAttribute(Qt::WA_DeleteOnClose);

    m_editor = new CodeEditor(this);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_editor); // page 0 -- see the seam note in the header

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    buildToolBar();
    m_layout->addWidget(m_toolBar);
    m_auxiliaryInsertIndex = m_layout->count(); // find bars land here
    m_layout->addWidget(m_stack, 1);

    connect(m_editor->document(), &QTextDocument::modificationChanged, this,
            &TextEditor::onModificationChanged);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this]() { save(); });

    updateModifiedIndicator();
}

void TextEditor::buildToolBar() {
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("TextEditorToolBar"));

    // Same idiom as QuickView's image/video/text toolbars: a plain QToolBar
    // with text actions, so the app's one QToolBar stylesheet rule dresses it.
    m_saveAction = m_toolBar->addAction(tr("Save"), this, [this]() { save(); });
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setToolTip(tr("Write the buffer to disk (Ctrl+S)"));
    m_saveAction->setEnabled(false); // nothing to write until something changes

    m_toolBar->addSeparator();

    m_encodingCombo = new QComboBox(m_toolBar);
    // A plain QListView popup honours the QSS `::item` colours; the platform's
    // native combo popup ignores them and paints from the palette Text role,
    // which turns non-selected rows invisible in the light theme. (Same reason
    // QuickView does this.)
    m_encodingCombo->setView(new QListView(m_encodingCombo));
    for (const TextEncoding &encoding : kTextEncodings)
        m_encodingCombo->addItem(QString::fromLatin1(encoding.label));
    m_encodingCombo->setToolTip(tr("Re-read the file on disk in this encoding"));
    m_toolBar->addWidget(m_encodingCombo);

    m_encodingStatus = new QLabel(m_toolBar);
    m_encodingStatus->setObjectName(QStringLiteral("textEncodingStatus"));
    m_toolBar->addWidget(m_encodingStatus);

    connect(m_encodingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &TextEditor::onEncodingSelected);

    auto *spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);

    m_modifiedLabel = new QLabel(m_toolBar);
    m_modifiedLabel->setObjectName(QStringLiteral("textEditorModified"));
    m_toolBar->addWidget(m_modifiedLabel);
}

void TextEditor::addAuxiliaryBar(QWidget *bar) {
    if (!bar)
        return;
    bar->setParent(this);
    m_layout->insertWidget(m_auxiliaryInsertIndex, bar);
    ++m_auxiliaryInsertIndex;
}

int TextEditor::addView(QWidget *view) {
    if (!view)
        return -1;
    return m_stack->addWidget(view);
}

void TextEditor::setCurrentView(int index) {
    if (index >= 0 && index < m_stack->count())
        m_stack->setCurrentIndex(index);
}

QString TextEditor::encodingStatusText() const {
    return m_encodingStatus ? m_encodingStatus->text() : QString();
}

QByteArray TextEditor::currentCodecName() const {
    const int index = m_encodingCombo ? m_encodingCombo->currentIndex() : kAutoEncodingIndex;
    if (index == kAutoEncodingIndex)
        return m_detected.codecName.isEmpty() ? QByteArrayLiteral("UTF-8") : m_detected.codecName;
    if (index < 0 || index >= kEncodingCount)
        return QByteArrayLiteral("UTF-8");
    const char *name = kTextEncodings[index].codec;
    if (name)
        return QByteArray(name);
    QTextCodec *locale = QTextCodec::codecForLocale();
    return locale ? locale->name() : QByteArrayLiteral("UTF-8");
}

bool TextEditor::loadFile(const QString &path) {
    QFileInfo info(path);
    if (info.size() > kMaxEditableBytes) {
        ttc::warning(this, tr("Edit"),
                              tr("%1 is too large to edit (over 50 MB).").arg(info.fileName()));
        return false;
    }

    // Read as bytes, not through a QTextStream: the stream would decode with a
    // codec of its own choosing and throw the original away, leaving nothing to
    // re-decode when the user picks a different encoding.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    m_raw = file.readAll();
    file.close();

    m_path = path;
    m_detected = detectForEditing(m_raw);

    QSignalBlocker blocker(m_encodingCombo);
    m_encodingCombo->setCurrentIndex(kAutoEncodingIndex);
    m_appliedEncodingIndex = kAutoEncodingIndex;
    blocker.unblock();

    applyEncodingToBuffer();
    updateTitle();
    return true;
}

void TextEditor::applyEncodingToBuffer() {
    const int index = m_encodingCombo->currentIndex();
    QString text;
    if (index == kAutoEncodingIndex) {
        text = TextEncodingDetector::decode(m_raw, m_detected);
        QString status = tr("Auto: %1").arg(m_detected.label);
        if (m_detected.binary)
            status = tr("Auto: Binary");
        else if (m_detected.ambiguous)
            status += tr(" (ambiguous)");
        m_encodingStatus->setText(status);
    } else {
        const char *codecName = kTextEncodings[index].codec;
        QTextCodec *codec =
            codecName ? QTextCodec::codecForName(codecName) : QTextCodec::codecForLocale();
        if (!codec)
            codec = QTextCodec::codecForName("UTF-8");
        // A default ConverterState consumes a leading BOM instead of turning it
        // into a stray U+FEFF at the top of the buffer.
        QTextCodec::ConverterState state;
        text = codec ? codec->toUnicode(m_raw.constData(), m_raw.size(), &state)
                     : QString::fromUtf8(m_raw);
        m_encodingStatus->setText(
            tr("Manual: %1").arg(QString::fromLatin1(kTextEncodings[index].label)));
    }

    // Sampled BEFORE the text reaches the document, which is the last moment a
    // CR still exists (see the m_usesCrlf note in the header).
    m_usesCrlf = text.contains(QLatin1String("\r\n"));

    m_editor->setPlainText(text);
    m_editor->document()->setModified(false);
    m_appliedEncodingIndex = index;
    updateTitle();
    updateModifiedIndicator();
}

void TextEditor::onEncodingSelected(int index) {
    if (index == m_appliedEncodingIndex)
        return;

    // Changing the encoding re-decodes the bytes on disk, which necessarily
    // replaces the buffer -- there is no way to reinterpret edited text without
    // guessing at bytes that were never written. Rather than silently dropping
    // the user's work, or writing it out in the OLD encoding behind their back
    // (a save they never asked for, in a codec they were in the middle of
    // rejecting), the change is confirmed and otherwise abandoned.
    if (m_editor->document()->isModified()) {
        const auto answer =
            ttc::question(this, tr("Change Encoding"),
                          tr("Changing the encoding re-reads %1 from disk and discards your "
                             "unsaved changes.\n\nSave first if you want to keep them.")
                              .arg(QFileInfo(m_path).fileName()),
                          QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            QSignalBlocker blocker(m_encodingCombo);
            m_encodingCombo->setCurrentIndex(m_appliedEncodingIndex);
            return;
        }
    }

    // Re-read the file rather than trusting the copy in memory: an encoding
    // change is the one moment the user is explicitly asking "show me what is
    // actually in that file". If the re-read fails (the file was removed or
    // the mount died), keep the bytes already held so the window is never left
    // showing nothing.
    QFile file(m_path);
    if (file.open(QIODevice::ReadOnly)) {
        m_raw = file.readAll();
        file.close();
        m_detected = detectForEditing(m_raw);
    }

    applyEncodingToBuffer();
}

QByteArray TextEditor::encodeBuffer() const {
    QString text = m_editor->toPlainText();
    if (m_usesCrlf)
        text.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    const int index = m_encodingCombo->currentIndex();

    if (index == kAutoEncodingIndex) {
        // Auto means "the encoding this file already was", so the file's own
        // BOM bytes are put back verbatim and the encoder is told not to add
        // one of its own. A UTF-8-with-BOM file therefore stays UTF-8-with-BOM
        // instead of quietly losing its signature on the first save.
        QTextCodec *codec = QTextCodec::codecForName(
            m_detected.codecName.isEmpty() ? QByteArrayLiteral("UTF-8") : m_detected.codecName);
        if (!codec)
            codec = QTextCodec::codecForName("UTF-8");
        QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
        QByteArray bytes = codec->fromUnicode(text.constData(), text.size(), &state);
        if (m_detected.bomBytes > 0 && m_detected.bomBytes <= m_raw.size())
            bytes.prepend(m_raw.left(m_detected.bomBytes));
        return bytes;
    }

    // A manual choice is a deliberate conversion, so the original file's BOM is
    // not carried over -- it would describe the wrong encoding. The codec's own
    // encoder decides instead, which is what emits the BOM that UTF-16/UTF-32
    // need to be readable at all and emits none for the 8-bit codecs.
    const char *codecName = kTextEncodings[index].codec;
    QTextCodec *codec =
        codecName ? QTextCodec::codecForName(codecName) : QTextCodec::codecForLocale();
    if (!codec)
        codec = QTextCodec::codecForName("UTF-8");
    return codec ? codec->fromUnicode(text) : text.toUtf8();
}

bool TextEditor::save() {
    if (m_path.isEmpty())
        return false;

    const QByteArray bytes = encodeBuffer();

    // Same call as before: a plain QFile on the path handed to loadFile(). The
    // permission and elevation behaviour is whatever opening that path gives,
    // unchanged. Only QIODevice::Text is gone, so the bytes written are exactly
    // the bytes encodeBuffer() produced.
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ttc::warning(this, tr("Save"), tr("Could not write to %1").arg(m_path));
        return false;
    }
    const qint64 written = file.write(bytes);
    const bool flushed = file.flush();
    file.close();
    if (written != bytes.size() || !flushed) {
        ttc::warning(this, tr("Save"), tr("Could not write to %1").arg(m_path));
        return false;
    }

    // The bytes on disk are now these, so a later encoding change re-decodes
    // the saved file and not the one that was opened.
    m_raw = bytes;
    m_detected = detectForEditing(m_raw);
    m_editor->document()->setModified(false);
    return true;
}

void TextEditor::onModificationChanged(bool) {
    updateTitle();
    updateModifiedIndicator();
}

void TextEditor::updateModifiedIndicator() {
    const bool modified = m_editor->document()->isModified();
    if (m_saveAction)
        m_saveAction->setEnabled(modified);
    if (m_modifiedLabel)
        m_modifiedLabel->setText(modified ? tr("Modified") : QString());
}

void TextEditor::updateTitle() {
    const QString name = QFileInfo(m_path).fileName();
    setWindowTitle(m_editor->document()->isModified() ? name + QStringLiteral(" *") : name);
}

bool TextEditor::promptSaveIfModified() {
    if (!m_editor->document()->isModified())
        return true;
    const auto answer = ttc::question(
        this, tr("Unsaved Changes"), tr("Save changes to %1?").arg(QFileInfo(m_path).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        save();
    return true;
}

void TextEditor::closeEvent(QCloseEvent *event) {
    if (promptSaveIfModified())
        event->accept();
    else
        event->ignore();
}
