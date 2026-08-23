#include "TextEditor.h"

#include "config/Settings.h"
#include "text/TextEncodingIdentity.h"
#include "FindBar.h"
#include "HexEditor.h"
#include "BinarySniff.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextCodec>
#include <QTimer>
#include <QToolBar>

#include "ThemedDialogs.h"
#include <QPainter>
#include <QShortcut>
#include <QTextBlock>
#include <QVBoxLayout>

namespace {

// Encodings the user can force on the open file, and the same table QuickView's
// preview toolbar offers -- it lives in core/text next to the detector that the
// Auto row defers to. Aliased locally because the code below indexes it in
// several places.
using TextEncoding = TextEncodingDetector::Selectable;
constexpr auto &kTextEncodings = TextEncodingDetector::selectableEncodings;
constexpr int kAutoEncodingIndex = TextEncodingDetector::autoEncodingIndex;
constexpr int kEncodingCount = TextEncodingDetector::selectableEncodingCount;

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

TextEditor::TextEditor(QWidget *parent) : TextEditor(nullptr, parent) {}

TextEditor::TextEditor(Settings &settings, QWidget *parent) : TextEditor(&settings, parent) {}

TextEditor::TextEditor(Settings *settings, QWidget *parent)
    : FramelessWindow(parent), m_settings(settings) {
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
    connect(m_editor->document(), &QTextDocument::contentsChanged, this,
            &TextEditor::onContentChanged);

    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setObjectName(QStringLiteral("textEditorAutoSaveTimer"));
    m_autoSaveTimer->setSingleShot(true);
    m_autoSaveTimer->setInterval(kAutoSaveDelayMs);
    connect(m_autoSaveTimer, &QTimer::timeout, this, [this]() { flushAutoSave(); });

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

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, &TextEditor::showFindBar);
    // F3 is what the quick-view window uses, so the two windows agree.
    auto *findAgain = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(findAgain, &QShortcut::activated, this, [this]() {
        if (m_findBar && m_findBar->isVisible())
            m_findBar->repeatSearch(ByteSearch::Direction::Forward);
        else
            showFindBar();
    });

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

    connect(m_encodingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &TextEditor::onEncodingSelected);

    m_partialLabel = new QLabel(m_toolBar);
    m_partialLabel->setObjectName(QStringLiteral("textEditorPartial"));
    m_toolBar->addWidget(m_partialLabel);
    m_loadRemainderAction = m_toolBar->addAction(tr("Load remainder"), this,
                                                 &TextEditor::loadRemainder);
    m_loadRemainderAction->setToolTip(tr("Load the rest of this file"));
    m_loadRemainderAction->setEnabled(false);

    auto *spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);

    m_modifiedLabel = new QLabel(m_toolBar);
    m_modifiedLabel->setObjectName(QStringLiteral("textEditorModified"));
    m_toolBar->addWidget(m_modifiedLabel);

    // Far right, mirroring the preview's Edit button. The host switches back to
    // the preview: this library sits below the UI layer and does not know about
    // it. Flush a still-pending autosave first because going back re-reads the
    // file from disk.
    m_previewAction = m_toolBar->addAction(tr("Preview"), this, [this]() {
        if (!m_path.isEmpty() && promptSaveIfModified())
            emit previewRequested(m_path);
    });
    m_previewAction->setToolTip(tr("Go back to the preview"));
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
    return m_encodingCombo ? m_encodingCombo->currentText() : QString();
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
    return loadFile(path, QString());
}

bool TextEditor::loadFile(const QString &path, const QString &encodingIdentity) {
    if (!m_path.isEmpty() && isDocumentModified() && !promptSaveIfModified())
        return false;

    // Files beyond the preview-sized window keep just a safe prefix. The
    // few extra bytes let text safePrefix end on a complete encoded character.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const qint64 fileSize = file.size();
    m_partiallyLoaded = fileSize > kLazyLoadBytes;
    m_raw = m_partiallyLoaded ? file.read(kLazyLoadBytes + kTextReadLookAheadBytes)
                              : file.readAll();
    m_loadedBytes = m_raw.size();

    resetAutoSaveState();
    m_path = path;
    m_encodingIdentity = encodingIdentity.isEmpty()
                             ? fc::TextEncodingIdentity::localPath(path)
                             : encodingIdentity;

    // Text or hex, decided from the bytes rather than the extension. A file
    // that is not text must not be decoded into a buffer, because saving it
    // would re-encode the decoder's guesses and write a different file back.
    m_hexMode = fc::shouldEditAsHex(m_raw.left(fc::sniffSampleBytes()), path);
    file.close();

    if (m_hexMode) {
        if (!m_hex) {
            m_hex = new HexEditor(this);
            addView(m_hex);
            // onModificationChanged, not just the indicator: the title's star
            // and the toolbar have to move together, and the text document's
            // own signal already goes through there.
            connect(m_hex, &HexEditor::modificationChanged, this,
                    &TextEditor::onModificationChanged);
            connect(m_hex, &HexEditor::contentsChanged, this,
                    &TextEditor::onContentChanged);
        }
        m_installingContent = true;
        m_hex->setContents(m_raw);
        m_hex->setModified(false);
        m_installingContent = false;
        setCurrentView(viewStack()->indexOf(m_hex));
        // Nothing to decode and nothing to choose: the encoding controls
        // describe a text buffer that does not exist in this mode.
        m_encodingCombo->setEnabled(false);
        QSignalBlocker hexBlocker(m_encodingCombo);
        m_encodingCombo->setItemText(kAutoEncodingIndex, tr("Binary (hex)"));
        m_encodingCombo->setCurrentIndex(kAutoEncodingIndex);
        updatePartialIndicator();
        updateTitle();
        updateModifiedIndicator();
        return true;
    }
    if (m_partiallyLoaded) {
        m_detected = TextEncodingDetector::detect(m_raw,
                                                  TextEncodingDetector::InputEnd::MayBeTruncated);
        m_raw = TextEncodingDetector::safePrefix(m_raw, static_cast<int>(kLazyLoadBytes),
                                                 m_detected);
        m_loadedBytes = m_raw.size();
    } else {
        m_detected = TextEncodingDetector::detect(m_raw);
    }
    m_encodingCombo->setEnabled(true);
    setCurrentView(0);

    const int remembered = m_settings
                               ? m_settings->rememberedTextEncodingIndex(m_encodingIdentity)
                               : kAutoEncodingIndex;
    QSignalBlocker blocker(m_encodingCombo);
    m_encodingCombo->setCurrentIndex(remembered);
    m_appliedEncodingIndex = remembered;
    blocker.unblock();

    applyEncodingToBuffer();
    updateTitle();
    return true;
}

void TextEditor::updatePartialIndicator() {
    if (m_partialLabel)
        m_partialLabel->setText(m_partiallyLoaded ? tr("Partially loaded") : QString());
    if (m_loadRemainderAction)
        m_loadRemainderAction->setEnabled(m_partiallyLoaded && !m_hexMode);
}

void TextEditor::loadRemainder() {
    if (!m_partiallyLoaded || m_path.isEmpty())
        return;
    // Keep a pending prefix edit before replacing the visible document with
    // the complete file. writePartialBuffer streams the old tail unchanged.
    if (isDocumentModified() && !save())
        return;

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        showSaveFailure();
        return;
    }
    m_raw = file.readAll();
    file.close();
    m_loadedBytes = m_raw.size();
    m_partiallyLoaded = false;
    m_detected = TextEncodingDetector::detect(m_raw);
    applyEncodingToBuffer();
}

void TextEditor::applyEncodingToBuffer() {
    const int index = m_encodingCombo->currentIndex();
    QString text;
    if (index == kAutoEncodingIndex) {
        text = TextEncodingDetector::decode(m_raw, m_detected);
        // What Auto resolved to goes into the Auto row itself, so the combo
        // alone tells the whole story and no second widget is needed.
        QString autoLabel = tr("Auto (%1)").arg(m_detected.label);
        if (m_detected.binary)
            autoLabel = tr("Auto (Binary)");
        else if (m_detected.ambiguous)
            autoLabel = tr("Auto (%1, ambiguous)").arg(m_detected.label);
        m_encodingCombo->setItemText(kAutoEncodingIndex, autoLabel);
    } else {
        QTextCodec *codec = TextEncodingDetector::codecForSelectableIndex(index);
        // A default ConverterState consumes a leading BOM instead of turning it
        // into a stray U+FEFF at the top of the buffer.
        QTextCodec::ConverterState state;
        text = codec ? codec->toUnicode(m_raw.constData(), m_raw.size(), &state)
                     : QString::fromUtf8(m_raw);
    }

    // Sampled BEFORE the text reaches the document, which is the last moment a
    // CR still exists (see the m_usesCrlf note in the header).
    m_usesCrlf = text.contains(QLatin1String("\r\n"));

    m_installingContent = true;
    m_editor->setPlainText(text);
    m_editor->document()->setModified(false);
    m_installingContent = false;
    resetAutoSaveState();
    m_appliedEncodingIndex = index;
    updatePartialIndicator();
    updateTitle();
    updateModifiedIndicator();
}

void TextEditor::onEncodingSelected(int index) {
    if (index == m_appliedEncodingIndex)
        return;
    if (m_autoSaveTimer)
        m_autoSaveTimer->stop();

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
            // The user kept the old encoding and the dirty buffer, so this is
            // still the same edit episode the timer was tracking.
            if (m_autoSaveTimer)
                m_autoSaveTimer->start();
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
        m_loadedBytes = m_raw.size();
        m_partiallyLoaded = false;
        m_detected = TextEncodingDetector::detect(m_raw);
    }

    if (m_settings)
        m_settings->setRememberedTextEncodingIndex(m_encodingIdentity, index);
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
    QTextCodec *codec = TextEncodingDetector::codecForSelectableIndex(index);
    return codec ? codec->fromUnicode(text) : text.toUtf8();
}

bool TextEditor::writePartialBuffer(const QByteArray &prefix) {
    // QSaveFile keeps the original file intact until its prefix and untouched
    // tail have both been copied. Direct truncate-and-write would destroy the
    // tail before it could be appended.
    QFile source(m_path);
    if (!source.open(QIODevice::ReadOnly) || source.size() < m_loadedBytes ||
        !source.seek(m_loadedBytes))
        return false;

    QSaveFile destination(m_path);
    if (!destination.open(QIODevice::WriteOnly) ||
        destination.write(prefix) != prefix.size())
        return false;

    qint64 tailBytes = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kTailCopyBytes);
        if (chunk.isEmpty()) {
            source.close();
            return false;
        }
        if (destination.write(chunk) != chunk.size()) {
            source.close();
            return false;
        }
        tailBytes += chunk.size();
    }
    source.close(); // Windows cannot replace an open source file.
    if (!destination.commit())
        return false;

    m_raw = prefix;
    m_loadedBytes = prefix.size();
    m_partiallyLoaded = tailBytes > 0;
    return true;
}

bool TextEditor::writeCurrentBuffer() {
    if (m_path.isEmpty())
        return false;

    // In hex mode the bytes ARE the document; encodeBuffer() would be encoding
    // a text buffer that was never decoded.
    const QByteArray bytes = m_hexMode ? m_hex->contents() : encodeBuffer();

    if (m_partiallyLoaded) {
        if (!writePartialBuffer(bytes))
            return false;
    } else {
        // Keep QFile here for fully materialised files: a network edit reaches
        // a writable GVfs/FUSE path, where QSaveFile's rename is not guaranteed
        // to have the same semantics as the existing direct write.
        QFile file(m_path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        const qint64 written = file.write(bytes);
        const bool flushed = file.flush();
        file.close();
        if (written != bytes.size() || !flushed)
            return false;

        m_raw = bytes;
        m_loadedBytes = m_raw.size();
        m_partiallyLoaded = false;
    }

    // The bytes on disk are now these, so a later encoding change re-decodes
    // the saved file and not the one that was opened.
    if (m_hexMode) {
        m_hex->setModified(false);
    } else {
        m_detected = TextEncodingDetector::detect(
            m_raw, m_partiallyLoaded ? TextEncodingDetector::InputEnd::MayBeTruncated
                                     : TextEncodingDetector::InputEnd::Complete);
        m_editor->document()->setModified(false);
    }
    resetAutoSaveState();
    updatePartialIndicator();
    updateTitle();
    updateModifiedIndicator();
    return true;
}

void TextEditor::showSaveFailure() {
    ttc::warning(this, tr("Save"), tr("Could not write to %1").arg(m_path));
}

bool TextEditor::flushAutoSave() {
    if (m_autoSaveTimer)
        m_autoSaveTimer->stop();
    if (!isDocumentModified())
        return true;
    if (m_autoSaveBlocked)
        return false;
    if (writeCurrentBuffer())
        return true;

    // One modal per failure episode. Repeated timer/close/Preview attempts stay
    // blocked until the user edits again or deliberately retries with Save.
    m_autoSaveBlocked = true;
    showSaveFailure();
    return false;
}

bool TextEditor::save() {
    if (m_path.isEmpty())
        return false;
    if (m_autoSaveTimer)
        m_autoSaveTimer->stop();
    if (writeCurrentBuffer())
        return true;
    m_autoSaveBlocked = true;
    showSaveFailure();
    return false;
}

void TextEditor::showFindBar() {
    if (!m_findBar) {
        m_findBar = new FindBar(this);
        addAuxiliaryBar(m_findBar);
        connect(m_findBar, &FindBar::searchRequested, this, &TextEditor::runSearch);
        connect(m_findBar, &FindBar::queryChanged, this,
                [this](const ByteSearch::Needle &) { m_lastMatchOffset = -1; });
        connect(m_findBar, &FindBar::closed, this, [this]() {
            m_lastMatchOffset = -1;
            (m_hexMode ? static_cast<QWidget *>(m_hex) : m_editor)->setFocus();
        });
    }
    // The needle is encoded with the encoding the file is being read in, so the
    // bar has to be told when that changes -- searching UTF-8 bytes for a
    // UTF-16-encoded word finds nothing.
    m_findBar->setEncoding(m_hexMode ? QByteArray("UTF-8") : currentCodecName());
    m_findBar->activate();
}

void TextEditor::runSearch(const ByteSearch::Needle &needle, ByteSearch::Direction direction) {
    // Always the file's bytes, never the decoded text: that is the whole point
    // of searching here rather than in the text document, and it is what makes
    // one find bar serve both views.
    const QByteArray &hay = m_hexMode ? m_hex->contents() : m_raw;
    const int from = m_lastMatchOffset < 0
                         ? (direction == ByteSearch::Direction::Forward ? 0 : hay.size())
                         : m_lastMatchOffset + (direction == ByteSearch::Direction::Forward ? 1 : 0);
    const int at = ByteSearch::find(hay, needle, from, direction, /*wrap=*/true);
    if (at == ByteSearch::kNotFound) {
        m_findBar->showNoMatch();
        return;
    }
    m_lastMatchOffset = at;
    m_findBar->showMatch(ByteSearch::ordinalAt(hay, needle, at),
                         ByteSearch::countMatches(hay, needle));
    if (m_hexMode) {
        m_hex->selectRange(at, needle.bytes.size());
        return;
    }
    // A text view is addressed in characters, not bytes, so the byte offset has
    // to be converted through the same codec the buffer was decoded with --
    // decoding the prefix is the only way that holds for a variable-width
    // encoding.
    QTextCodec *codec = QTextCodec::codecForName(currentCodecName());
    if (!codec)
        return;
    const int chars = codec->toUnicode(hay.left(at)).size();
    const int length = codec->toUnicode(hay.mid(at, needle.bytes.size())).size();
    QTextCursor cursor = m_editor->textCursor();
    cursor.setPosition(chars);
    cursor.setPosition(chars + length, QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
    m_editor->ensureCursorVisible();
}

void TextEditor::onModificationChanged(bool) {
    updateTitle();
    updateModifiedIndicator();
}

void TextEditor::onContentChanged() {
    if (m_installingContent || m_path.isEmpty())
        return;
    // A real edit starts a new failure episode and moves the deadline so the
    // timer always writes the latest point in the typing burst.
    m_autoSaveBlocked = false;
    if (m_autoSaveTimer)
        m_autoSaveTimer->start();
}

void TextEditor::resetAutoSaveState() {
    if (m_autoSaveTimer)
        m_autoSaveTimer->stop();
    m_autoSaveBlocked = false;
}

void TextEditor::updateModifiedIndicator() {
    const bool modified = isDocumentModified();
    if (m_saveAction)
        m_saveAction->setEnabled(modified);
    if (m_modifiedLabel)
        m_modifiedLabel->setText(modified ? tr("Modified") : QString());
}

bool TextEditor::isDocumentModified() const {
    // Whichever view holds the document holds the modification with it. Reading
    // the text document unconditionally would leave hex edits invisible: Save
    // would stay greyed out, the title would lose its star, and closing would
    // discard the edits without asking.
    return m_hexMode ? (m_hex && m_hex->isModified()) : m_editor->document()->isModified();
}

void TextEditor::updateTitle() {
    const QString name = QFileInfo(m_path).fileName();
    setWindowTitle(isDocumentModified() ? name + QStringLiteral(" *") : name);
}

bool TextEditor::promptSaveIfModified() {
    return !isDocumentModified() || flushAutoSave();
}

void TextEditor::closeEvent(QCloseEvent *event) {
    if (promptSaveIfModified())
        event->accept();
    else
        event->ignore();
}
