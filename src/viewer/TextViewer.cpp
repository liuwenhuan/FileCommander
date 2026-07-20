#include "TextViewer.h"

#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextCodec>
#include <QTextCursor>
#include <QToolBar>
#include <QVBoxLayout>

namespace {
struct Encoding {
    const char *label;
    const char *codec;
};
// codec == nullptr means "use the locale codec".
const Encoding kEncodings[] = {
    {"UTF-8", "UTF-8"},   {"UTF-16", "UTF-16"},         {"ISO-8859-1", "ISO-8859-1"},
    {"GB18030", "GB18030"}, {"Windows-1252", "Windows-1252"}, {"System", nullptr},
};
} // namespace

TextViewer::TextViewer(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);

    auto *toolbar = new QToolBar(this);

    m_encoding = new QComboBox(toolbar);
    for (const Encoding &e : kEncodings)
        m_encoding->addItem(QString::fromLatin1(e.label));
    toolbar->addWidget(m_encoding);
    connect(m_encoding, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { render(); });

    QAction *wrapAction = toolbar->addAction(tr("Wrap"));
    wrapAction->setCheckable(true);
    connect(wrapAction, &QAction::toggled, this, [this](bool on) {
        m_editor->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    });

    QAction *hexAction = toolbar->addAction(tr("Hex"));
    hexAction->setCheckable(true);
    connect(hexAction, &QAction::toggled, this, [this](bool on) {
        m_hex = on;
        m_encoding->setEnabled(!on); // encoding is meaningless in hex mode
        render();
    });

    toolbar->addSeparator();
    m_find = new QLineEdit(toolbar);
    m_find->setPlaceholderText(tr("Find… (Enter / F3)"));
    m_find->setClearButtonEnabled(true);
    toolbar->addWidget(m_find);
    connect(m_find, &QLineEdit::returnPressed, this, &TextViewer::findNext);

    m_editor = new QPlainTextEdit(this);
    m_editor->setReadOnly(true);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setFont(QFont(QStringLiteral("monospace")));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(toolbar);
    layout->addWidget(m_editor);

    auto *closeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(closeShortcut, &QShortcut::activated, this, &QWidget::close);
    auto *findShortcut = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(findShortcut, &QShortcut::activated, this, &TextViewer::findNext);
}

bool TextViewer::loadFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    m_raw = file.read(kMaxBytes);
    m_truncated = !file.atEnd();
    setWindowTitle(QFileInfo(path).fileName());
    render();
    return true;
}

QString TextViewer::toHexDump(const QByteArray &data) {
    QString out;
    out.reserve(data.size() * 4);
    for (int offset = 0; offset < data.size(); offset += 16) {
        out += QStringLiteral("%1  ").arg(offset, 8, 16, QLatin1Char('0'));
        QString ascii;
        for (int i = 0; i < 16; ++i) {
            if (offset + i < data.size()) {
                const uchar b = static_cast<uchar>(data.at(offset + i));
                out += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
                ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QLatin1Char('.');
            } else {
                out += QStringLiteral("   ");
            }
        }
        out += QLatin1Char(' ') + ascii + QLatin1Char('\n');
    }
    return out;
}

void TextViewer::render() {
    QString content;
    if (m_hex) {
        content = toHexDump(m_raw);
    } else {
        const char *codecName = kEncodings[m_encoding->currentIndex()].codec;
        QTextCodec *codec = codecName ? QTextCodec::codecForName(codecName)
                                       : QTextCodec::codecForLocale();
        if (!codec)
            codec = QTextCodec::codecForName("UTF-8");
        content = codec->toUnicode(m_raw);
    }
    if (m_truncated)
        content += QStringLiteral("\n\n[... truncated, file exceeds 5 MB ...]");
    m_editor->setPlainText(content);
}

void TextViewer::findNext() {
    const QString needle = m_find->text();
    if (needle.isEmpty())
        return;
    if (!m_editor->find(needle)) {
        // Wrap around to the top and try once more.
        m_editor->moveCursor(QTextCursor::Start);
        m_editor->find(needle);
    }
}
