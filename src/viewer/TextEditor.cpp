#include "TextEditor.h"

#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>

#include "ThemedDialogs.h"
#include <QPainter>
#include <QShortcut>
#include <QTextBlock>
#include <QTextStream>
#include <QVBoxLayout>

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
    painter.fillRect(event->rect(), palette().alternateBase());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
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

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor);

    connect(m_editor->document(), &QTextDocument::modificationChanged, this,
            &TextEditor::onModificationChanged);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &TextEditor::save);
}

bool TextEditor::loadFile(const QString &path) {
    QFileInfo info(path);
    if (info.size() > kMaxEditableBytes) {
        ttc::warning(this, tr("Edit"),
                              tr("%1 is too large to edit (over 50 MB).").arg(info.fileName()));
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    m_editor->setPlainText(stream.readAll());
    m_editor->document()->setModified(false);
    m_path = path;
    updateTitle();
    return true;
}

void TextEditor::save() {
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        ttc::warning(this, tr("Save"), tr("Could not write to %1").arg(m_path));
        return;
    }
    QTextStream stream(&file);
    stream << m_editor->toPlainText();
    file.close();
    m_editor->document()->setModified(false);
}

void TextEditor::onModificationChanged(bool) {
    updateTitle();
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
