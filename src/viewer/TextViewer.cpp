#include "TextViewer.h"

#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextStream>
#include <QVBoxLayout>

TextViewer::TextViewer(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);

    m_editor = new QPlainTextEdit(this);
    m_editor->setReadOnly(true);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont font(QStringLiteral("monospace"));
    m_editor->setFont(font);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor);

    auto *closeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(closeShortcut, &QShortcut::activated, this, &QWidget::close);
}

bool TextViewer::loadFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    QString content = stream.read(kMaxBytes);
    if (!stream.atEnd())
        content += QStringLiteral("\n\n[... truncated, file exceeds 5 MB ...]");

    m_editor->setPlainText(content);
    setWindowTitle(QFileInfo(path).fileName());
    return true;
}
